/*
 *
 * Copyright (C) 2019-2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "ze_peer.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sys/ioctl.h>

#if ZE_PEER_ENABLE_MPI
#include <mpi.h>
#endif

namespace {

constexpr const char *vmem_dev_path = "/dev/vmem";
constexpr int mpi_metadata_tag_offset = 0;
constexpr int mpi_copy_done_tag_offset = 1;
constexpr int mpi_cleanup_tag_offset = 2;
constexpr int mpi_get_pfn_retry_count = 3;

std::mutex ze_ipc_api_mutex;
std::mutex vmem_ioctl_mutex;
std::mutex mpi_get_pfn_mutex;

bool ze_peer_mpi_debug_enabled() {
  static int enabled = []() {
    const char *env = std::getenv("ZE_PEER_IPC_MPI_DEBUG");
    return (env != nullptr && std::strcmp(env, "0") != 0) ? 1 : 0;
  }();
  return enabled != 0;
}

void ze_peer_mpi_debug_log(int rank, const char *fmt, ...) {
  if (!ze_peer_mpi_debug_enabled()) {
    return;
  }

  std::fprintf(stderr, "[ZE_PEER_MPI][rank=%d][pid=%d] ", rank,
               static_cast<int>(getpid()));
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
}

struct vmem_pfn_list {
  int nents;
  unsigned long long addrs[8];
  size_t size[8];
};

struct vmem_open_handle_data {
  ze_ipc_mem_handle_t ipc_handle;
  int rank;
  int device_id;
  uint32_t domain;
  uint32_t bus;
  uint32_t device;
  uint32_t function;
  struct vmem_pfn_list pfn_list;
  int fd;
};

struct mpi_exchange_data {
  int rank;
  int device_id;
  int dma_buf_fd;
  struct vmem_pfn_list pfn_list;
};

#define ZE_PEER_VMEM_IOCTL_GET_PFN_LIST                                       \
  _IOWR('v', 0, struct vmem_open_handle_data)
#define ZE_PEER_VMEM_IOCTL_GET_IPC_HANDLE                                     \
  _IOWR('v', 1, struct vmem_open_handle_data)

int get_device_id(ze_device_handle_t device) {
  ze_device_properties_t props = {ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
  SUCCESS_OR_TERMINATE(zeDeviceGetProperties(device, &props));
  return static_cast<int>(props.deviceId);
}

bool get_device_dbdf(ze_device_handle_t device,
                     uint32_t *domain,
                     uint32_t *bus,
                     uint32_t *dev,
                     uint32_t *func) {
  if (domain == nullptr || bus == nullptr || dev == nullptr || func == nullptr) {
    return false;
  }

  *domain = 0;
  *bus = 0;
  *dev = 0;
  *func = 0;

#if defined(ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES)
  ze_pci_ext_properties_t pci_props = {};
  pci_props.stype = ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES;
  if (zeDevicePciGetPropertiesExt(device, &pci_props) == ZE_RESULT_SUCCESS) {
    *domain = pci_props.address.domain;
    *bus = pci_props.address.bus;
    *dev = pci_props.address.device;
    *func = pci_props.address.function;
    return true;
  }
#endif

  return false;
}

#if ZE_PEER_ENABLE_MPI
[[noreturn]] void mpi_abort_with_message(const std::string &message) {
  int initialized = 0;
  MPI_Initialized(&initialized);
  std::cerr << message << "\n";
  if (initialized) {
    MPI_Abort(MPI_COMM_WORLD, -1);
  }
  std::terminate();
}

void mpi_pair_sync_or_abort(int mpi_rank, int mpi_tag) {
  const int peer_rank = 1 - mpi_rank;
  int send_token = 1;
  int recv_token = 0;

  ze_peer_mpi_debug_log(mpi_rank,
                        "sync begin: peer_rank=%d tag=%d",
                        peer_rank,
                        mpi_tag);

  int ret = MPI_Sendrecv(&send_token,
                         1,
                         MPI_INT,
                         peer_rank,
                         mpi_tag,
                         &recv_token,
                         1,
                         MPI_INT,
                         peer_rank,
                         mpi_tag,
                         MPI_COMM_WORLD,
                         MPI_STATUS_IGNORE);

  ze_peer_mpi_debug_log(mpi_rank,
                        "sync end: peer_rank=%d tag=%d ret=%d recv_token=%d",
                        peer_rank,
                        mpi_tag,
                        ret,
                        recv_token);

  if (ret != MPI_SUCCESS) {
    mpi_abort_with_message("MPI_Sendrecv failed while synchronizing pair");
  }
}

void exchange_ipc_mpi(ZePeer *peer,
                      uint32_t local_device_id,
                      int mpi_rank,
                      void **remote_ipc_buffer,
                      int metadata_tag) {
  int vmem_fd = open(vmem_dev_path, O_RDWR);
  ze_peer_mpi_debug_log(mpi_rank,
                        "open %s => fd=%d local_device=%u metadata_tag=%d",
                        vmem_dev_path,
                        vmem_fd,
                        local_device_id,
                        metadata_tag);
  if (vmem_fd < 0) {
    mpi_abort_with_message(std::string("Failed to open ") + vmem_dev_path +
                           ": " + strerror(errno));
  }

  ze_ipc_mem_handle_t local_handle{};
  int local_dma_buf_fd = -1;
  vmem_open_handle_data local_data{};
  bool pfn_ready = false;

  for (int attempt = 0; attempt < mpi_get_pfn_retry_count; attempt++) {
    int ioctl_ret = 0;
    {
      // Serialize handle export + PFN query to avoid cross-thread fd races.
      std::lock_guard<std::mutex> export_lock(mpi_get_pfn_mutex);

      {
        std::lock_guard<std::mutex> ze_lock(ze_ipc_api_mutex);
        peer->benchmark->getIpcHandle(peer->ze_buffers[local_device_id],
                                      &local_handle);
      }

      memcpy(&local_dma_buf_fd, &local_handle, sizeof(local_dma_buf_fd));

      local_data = {};
      local_data.ipc_handle = local_handle;
      local_data.rank = mpi_rank;
      local_data.device_id =
          get_device_id(peer->benchmark->_devices[local_device_id]);
      if (!get_device_dbdf(peer->benchmark->_devices[local_device_id],
                           &local_data.domain,
                           &local_data.bus,
                           &local_data.device,
                           &local_data.function)) {
        close(vmem_fd);
        mpi_abort_with_message("Failed to query PCI DBDF from Level Zero");
      }
      local_data.fd = local_dma_buf_fd;

      {
        std::lock_guard<std::mutex> ioctl_lock(vmem_ioctl_mutex);
        ze_peer_mpi_debug_log(mpi_rank,
                              "ioctl GET_PFN_LIST begin: attempt=%d rank=%d "
                              "device_id=0x%x dbdf=%04x:%02x:%02x.%x dma_fd=%d",
                              attempt,
                              local_data.rank,
                              local_data.device_id,
                              local_data.domain,
                              local_data.bus,
                              local_data.device,
                              local_data.function,
                              local_data.fd);
        ioctl_ret =
            ioctl(vmem_fd, ZE_PEER_VMEM_IOCTL_GET_PFN_LIST, &local_data);
        ze_peer_mpi_debug_log(mpi_rank,
                              "ioctl GET_PFN_LIST end: attempt=%d ret=%d "
                              "errno=%d nents=%d",
                              attempt,
                              ioctl_ret,
                              errno,
                              local_data.pfn_list.nents);
      }

      {
        std::lock_guard<std::mutex> ze_lock(ze_ipc_api_mutex);
        peer->benchmark->putIpcHandle(local_handle);
      }
    }

    if (ioctl_ret == 0) {
      if (local_data.pfn_list.nents <= 0) {
        close(vmem_fd);
        mpi_abort_with_message("Local PFN list is empty");
      }
      pfn_ready = true;
      break;
    }

    const int ioctl_errno = errno;
    if (ioctl_errno == EBADF && (attempt + 1) < mpi_get_pfn_retry_count) {
      continue;
    }

    close(vmem_fd);
    mpi_abort_with_message(std::string("VMEM_IOCTL_GET_PFN_LIST failed: ") +
                           strerror(ioctl_errno));
  }

  if (!pfn_ready) {
    close(vmem_fd);
    mpi_abort_with_message("VMEM_IOCTL_GET_PFN_LIST failed after retries");
  }

  mpi_exchange_data send_data{};
  send_data.rank = mpi_rank;
  send_data.device_id = local_data.device_id;
  send_data.dma_buf_fd = local_dma_buf_fd;
  memcpy(&send_data.pfn_list, &local_data.pfn_list, sizeof(send_data.pfn_list));

  mpi_exchange_data recv_data{};
  int peer_rank = 1 - mpi_rank;
  ze_peer_mpi_debug_log(mpi_rank,
                        "metadata exchange begin: peer_rank=%d tag=%d "
                        "local_dma_fd=%d local_device_id=0x%x pfn_nents=%d",
                        peer_rank,
                        metadata_tag,
                        send_data.dma_buf_fd,
                        send_data.device_id,
                        send_data.pfn_list.nents);
  int mpi_ret = MPI_Sendrecv(&send_data,
                             sizeof(send_data),
                             MPI_BYTE,
                             peer_rank,
                             metadata_tag,
                             &recv_data,
                             sizeof(recv_data),
                             MPI_BYTE,
                             peer_rank,
                             metadata_tag,
                             MPI_COMM_WORLD,
                             MPI_STATUS_IGNORE);
  ze_peer_mpi_debug_log(mpi_rank,
                        "metadata exchange end: ret=%d remote_dma_fd=%d "
                        "remote_device_id=0x%x remote_pfn_nents=%d",
                        mpi_ret,
                        recv_data.dma_buf_fd,
                        recv_data.device_id,
                        recv_data.pfn_list.nents);
  if (mpi_ret != MPI_SUCCESS) {
    close(vmem_fd);
    mpi_abort_with_message("MPI_Sendrecv failed while exchanging IPC metadata");
  }

  vmem_open_handle_data remote_data{};
  remote_data.rank = mpi_rank;
  remote_data.device_id = recv_data.device_id;
  memcpy(&remote_data.pfn_list, &recv_data.pfn_list,
         sizeof(remote_data.pfn_list));

  int open_ioctl_ret = 0;
  {
    std::lock_guard<std::mutex> lock(vmem_ioctl_mutex);
    ze_peer_mpi_debug_log(mpi_rank,
                          "ioctl GET_IPC_HANDLE begin: remote_device_id=0x%x "
                          "remote_pfn_nents=%d",
                          remote_data.device_id,
                          remote_data.pfn_list.nents);
    open_ioctl_ret = ioctl(vmem_fd, ZE_PEER_VMEM_IOCTL_GET_IPC_HANDLE,
                           &remote_data);
    ze_peer_mpi_debug_log(mpi_rank,
                          "ioctl GET_IPC_HANDLE end: ret=%d errno=%d new_fd=%d",
                          open_ioctl_ret,
                          errno,
                          remote_data.fd);
  }
  if (open_ioctl_ret != 0) {
    const int open_errno = errno;
    close(vmem_fd);
    mpi_abort_with_message(std::string("VMEM_IOCTL_GET_IPC_HANDLE failed: ") +
                           strerror(open_errno));
  }

  int remote_dma_buf_fd = remote_data.fd;
  if (remote_dma_buf_fd < 0) {
    close(vmem_fd);
    mpi_abort_with_message("VMEM_IOCTL_GET_IPC_HANDLE returned invalid dma-buf fd");
  }
  ze_ipc_mem_handle_t remote_handle{};
  memcpy(&remote_handle, &remote_dma_buf_fd, sizeof(remote_dma_buf_fd));

  {
    std::lock_guard<std::mutex> lock(ze_ipc_api_mutex);
    ze_peer_mpi_debug_log(mpi_rank,
                          "memoryOpenIpcHandle begin: local_device=%u",
                          local_device_id);
    peer->benchmark->memoryOpenIpcHandle(local_device_id,
                                         remote_handle,
                                         remote_ipc_buffer);
    ze_peer_mpi_debug_log(mpi_rank,
                          "memoryOpenIpcHandle end: local_device=%u remote_ptr=%p",
                          local_device_id,
                          *remote_ipc_buffer);
  }

  close(remote_dma_buf_fd);

  close(vmem_fd);
}
#endif

} // namespace

int ZePeer::sendmsg_fd(int socket, int fd) {
  char sendBuf[sizeof(ze_ipc_mem_handle_t)] = {};
  char cmsgBuf[CMSG_SPACE(sizeof(ze_ipc_mem_handle_t))];

  struct iovec msgBuffer;
  msgBuffer.iov_base = sendBuf;
  msgBuffer.iov_len = sizeof(*sendBuf);

  struct msghdr msgHeader = {};
  msgHeader.msg_iov = &msgBuffer;
  msgHeader.msg_iovlen = 1;
  msgHeader.msg_control = cmsgBuf;
  msgHeader.msg_controllen = CMSG_LEN(sizeof(fd));

  struct cmsghdr *controlHeader = CMSG_FIRSTHDR(&msgHeader);
  controlHeader->cmsg_type = SCM_RIGHTS;
  controlHeader->cmsg_level = SOL_SOCKET;
  controlHeader->cmsg_len = CMSG_LEN(sizeof(fd));

  *(int *)CMSG_DATA(controlHeader) = fd;
  ssize_t bytesSent = sendmsg(socket, &msgHeader, 0);
  if (bytesSent < 0) {
    return -1;
  }

  return 0;
}

int ZePeer::recvmsg_fd(int socket) {
  int fd = -1;
  char recvBuf[sizeof(ze_ipc_mem_handle_t)] = {};
  char cmsgBuf[CMSG_SPACE(sizeof(ze_ipc_mem_handle_t))];

  struct iovec msgBuffer;
  msgBuffer.iov_base = recvBuf;
  msgBuffer.iov_len = sizeof(recvBuf);

  struct msghdr msgHeader = {};
  msgHeader.msg_iov = &msgBuffer;
  msgHeader.msg_iovlen = 1;
  msgHeader.msg_control = cmsgBuf;
  msgHeader.msg_controllen = CMSG_LEN(sizeof(fd));

  ssize_t bytesSent = recvmsg(socket, &msgHeader, 0);
  if (bytesSent < 0) {
    return -1;
  }

  struct cmsghdr *controlHeader = CMSG_FIRSTHDR(&msgHeader);
  if (!controlHeader) {
    std::cerr << "Error receiving ipc handle";
    std::terminate();
  }
  memmove(&fd, CMSG_DATA(controlHeader), sizeof(int));
  return fd;
}

void ZePeer::set_up_ipc(size_t number_buffer_elements,
                        uint32_t device_id,
                        size_t &buffer_size,
                        ze_command_queue_handle_t &command_queue,
                        ze_command_list_handle_t &command_list) {
  size_t element_size = sizeof(char);
  buffer_size = element_size * number_buffer_elements;

  void *ze_buffer = nullptr;
  benchmark->memoryAlloc(device_id, buffer_size, &ze_buffer);
  ze_buffers[device_id] = ze_buffer;

  void **host_buffer = reinterpret_cast<void **>(&ze_host_buffer);
  benchmark->memoryAllocHost(buffer_size, host_buffer);
  void **host_validate_buffer =
      reinterpret_cast<void **>(&ze_host_validate_buffer);
  benchmark->memoryAllocHost(buffer_size, host_validate_buffer);

  command_list = ze_peer_devices[device_id].engines[0].second;
  command_queue = ze_peer_devices[device_id].engines[0].first;
}

void ZePeer::bandwidth_latency_ipc(peer_test_t test_type,
                                   peer_transfer_t transfer_type,
                                   bool is_server,
                                   int commSocket,
                                   size_t number_buffer_elements,
                                   uint32_t local_device_id,
                                   uint32_t remote_device_id,
                                   bool use_mpi_remote_exchange,
                                   int mpi_msg_tag_base) {
  size_t buffer_size = 0;
  ze_command_queue_handle_t command_queue = {};
  ze_command_list_handle_t command_list = {};
  void *remote_ipc_buffer = nullptr;
#if ZE_PEER_ENABLE_MPI
  int mpi_rank = 0;
#endif

  set_up_ipc(number_buffer_elements, local_device_id, buffer_size,
             command_queue, command_list);

  if (!is_server) {
    if (transfer_type == PEER_READ) {
      initialize_buffers(command_list, command_queue, nullptr, ze_host_buffer,
                         buffer_size);
    } else {
      initialize_buffers(command_list, command_queue,
                         ze_buffers[local_device_id], ze_host_buffer,
                         buffer_size);
    }
  } else {
    if (transfer_type == PEER_READ) {
      initialize_buffers(command_list, command_queue,
                         ze_buffers[local_device_id], ze_host_buffer,
                         buffer_size);
    } else {
      initialize_buffers(command_list, command_queue, nullptr, ze_host_buffer,
                         buffer_size);
    }
  }

  if (use_mpi_remote_exchange) {
#if ZE_PEER_ENABLE_MPI
    int mpi_size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &mpi_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    ze_peer_mpi_debug_log(mpi_rank,
                          "ipc_mpi entry: mpi_size=%d local_device=%u "
                          "remote_device=%u tag_base=%d",
                          mpi_size,
                          local_device_id,
                          remote_device_id,
                          mpi_msg_tag_base);
    if (mpi_size != 2) {
      mpi_abort_with_message("MPI mode requires exactly 2 ranks");
    }

    exchange_ipc_mpi(this,
                     local_device_id,
                     mpi_rank,
                     &remote_ipc_buffer,
                     mpi_msg_tag_base + mpi_metadata_tag_offset);
#else
    std::cerr << "MPI support is not available in this build\n";
    std::terminate();
#endif
  } else if (!is_server) {
    int dma_buf_fd = recvmsg_fd(commSocket);
    if (dma_buf_fd < 0) {
      std::cerr << "Failing to get dma_buf fd from server\n";
      std::terminate();
    }

    ze_ipc_mem_handle_t ipc_handle{};
    memcpy(&ipc_handle, &dma_buf_fd, sizeof(dma_buf_fd));
    {
      std::lock_guard<std::mutex> lock(ze_ipc_api_mutex);
      benchmark->memoryOpenIpcHandle(local_device_id, ipc_handle,
                                     &remote_ipc_buffer);
    }
    close(dma_buf_fd);
  } else {
    {
      std::lock_guard<std::mutex> lock(ze_ipc_api_mutex);
      benchmark->getIpcHandle(ze_buffers[local_device_id], &pIpcHandle);
    }
    int dma_buf_fd = -1;
    memcpy(&dma_buf_fd, &pIpcHandle, sizeof(dma_buf_fd));
    if (sendmsg_fd(commSocket, dma_buf_fd) < 0) {
      {
        std::lock_guard<std::mutex> lock(ze_ipc_api_mutex);
        benchmark->putIpcHandle(pIpcHandle);
      }
      std::cerr << "Failing to send dma_buf fd to client\n";
      std::terminate();
    }
    {
      std::lock_guard<std::mutex> lock(ze_ipc_api_mutex);
      benchmark->putIpcHandle(pIpcHandle);
    }
  }

  if (!is_server) {
    if (!remote_ipc_buffer) {
      std::cerr << "Remote IPC buffer was not initialized\n";
      std::terminate();
    }

    if (transfer_type == PEER_READ) {
      ze_peer_mpi_debug_log(mpi_rank,
                            "perform_copy begin (READ): local_device=%u "
                            "remote_device=%u size=%zu",
                            local_device_id,
                            remote_device_id,
                            buffer_size);
      perform_copy(test_type,
                   command_list,
                   command_queue,
                   ze_buffers[local_device_id],
                   remote_ipc_buffer,
                   buffer_size);
      ze_peer_mpi_debug_log(mpi_rank,
                            "perform_copy end (READ): local_device=%u "
                            "remote_device=%u size=%zu",
                            local_device_id,
                            remote_device_id,
                            buffer_size);

      if (validate_results) {
        validate_buffer(command_list,
                        command_queue,
                        ze_host_validate_buffer,
                        ze_buffers[local_device_id],
                        ze_host_buffer,
                        buffer_size);
      }
    } else {
      ze_peer_mpi_debug_log(mpi_rank,
                            "perform_copy begin (WRITE): local_device=%u "
                            "remote_device=%u size=%zu",
                            local_device_id,
                            remote_device_id,
                            buffer_size);
      perform_copy(test_type,
                   command_list,
                   command_queue,
                   remote_ipc_buffer,
                   ze_buffers[local_device_id],
                   buffer_size);
      ze_peer_mpi_debug_log(mpi_rank,
                            "perform_copy end (WRITE): local_device=%u "
                            "remote_device=%u size=%zu",
                            local_device_id,
                            remote_device_id,
                            buffer_size);
    }
  }

  if (use_mpi_remote_exchange) {
#if ZE_PEER_ENABLE_MPI
    ze_peer_mpi_debug_log(mpi_rank,
                "copy_done sync start: tag=%d",
                mpi_msg_tag_base + mpi_copy_done_tag_offset);
    mpi_pair_sync_or_abort(mpi_rank, mpi_msg_tag_base + mpi_copy_done_tag_offset);
    ze_peer_mpi_debug_log(mpi_rank,
                "copy_done sync end: tag=%d",
                mpi_msg_tag_base + mpi_copy_done_tag_offset);
#endif
  } else if (is_server) {
    int child_status = 0;
    pid_t client_pid = wait(&child_status);
    if (client_pid <= 0) {
      std::cerr << "Client terminated abruptly with error code "
                << strerror(errno) << "\n";
      std::terminate();
    }
  }

  if (is_server && transfer_type == PEER_WRITE && validate_results) {
    validate_buffer(command_list,
                    command_queue,
                    ze_host_validate_buffer,
                    ze_buffers[local_device_id],
                    ze_host_buffer,
                    buffer_size);
  }

  if (use_mpi_remote_exchange) {
#if ZE_PEER_ENABLE_MPI
    ze_peer_mpi_debug_log(mpi_rank,
                          "cleanup sync start: tag=%d",
                          mpi_msg_tag_base + mpi_cleanup_tag_offset);
    mpi_pair_sync_or_abort(mpi_rank, mpi_msg_tag_base + mpi_cleanup_tag_offset);
    ze_peer_mpi_debug_log(mpi_rank,
                          "cleanup sync end: tag=%d",
                          mpi_msg_tag_base + mpi_cleanup_tag_offset);
#endif
  }

  if (remote_ipc_buffer) {
    std::lock_guard<std::mutex> lock(ze_ipc_api_mutex);
    benchmark->closeIpcHandle(remote_ipc_buffer);
  }

  benchmark->memoryFree(ze_buffers[local_device_id]);
  benchmark->memoryFree(ze_host_buffer);
  benchmark->memoryFree(ze_host_validate_buffer);
}