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
#include <mutex>

#include <level_zero/zes_api.h>

#ifndef ZES_STRUCTURE_TYPE_PCI_PROPERTIES
#define ZES_STRUCTURE_TYPE_PCI_PROPERTIES static_cast<zes_structure_type_t>(0x2)
#endif

#if ZE_PEER_ENABLE_MPI
#include <dlfcn.h>
#include <mpi.h>
#include <vmem_test/vmem_lib.h>
#endif

namespace {

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

#if ZE_PEER_ENABLE_MPI
struct mpi_exchange_data {
  int dma_buf_fd;
  struct pfn_list pfn_list;
};

struct vmem_runtime_api {
  void *lib_handle;
  int (*open_fn)();
  int (*close_fn)(int);
  int (*init_fn)(uint32_t, uint32_t, uint32_t, uint32_t);
  int (*open_handle_fn)(int, ze_ipc_mem_handle_t *, struct pfn_list *);
  int (*get_handle_fn)(int, int *, struct pfn_list *);
  bool loaded;
  std::string load_error;
};

vmem_runtime_api &get_vmem_runtime_api() {
  static vmem_runtime_api api = {};
  static std::once_flag once;

  std::call_once(once, [&]() {
    const char *env_path = std::getenv("ZE_PEER_VMEM_LIB_PATH");
    const char *candidates[] = {
        env_path,
        "libvmem.so",
        "libvmem_lib.so",
    };

    auto load_symbol = [](void *handle, const char *name) -> void * {
      dlerror();
      void *sym = dlsym(handle, name);
      return (dlerror() == nullptr) ? sym : nullptr;
    };

    for (const char *candidate : candidates) {
      if (candidate == nullptr || candidate[0] == '\0') {
        continue;
      }

      void *handle = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
      if (handle == nullptr) {
        api.load_error = dlerror() ? dlerror() : "unknown dlopen error";
        continue;
      }

      api.open_fn = reinterpret_cast<int (*)()>(load_symbol(handle, "vmem_open"));
      api.close_fn = reinterpret_cast<int (*)(int)>(load_symbol(handle, "vmem_close"));
      api.init_fn = reinterpret_cast<int (*)(uint32_t, uint32_t, uint32_t, uint32_t)>(
          load_symbol(handle, "vmem_init"));
      api.open_handle_fn = reinterpret_cast<int (*)(int, ze_ipc_mem_handle_t *, struct pfn_list *)>(
          load_symbol(handle, "vmem_open_handle"));
      api.get_handle_fn = reinterpret_cast<int (*)(int, int *, struct pfn_list *)>(
          load_symbol(handle, "vmem_get_handle"));

      if (api.open_fn != nullptr && api.close_fn != nullptr && api.init_fn != nullptr &&
          api.open_handle_fn != nullptr && api.get_handle_fn != nullptr) {
        api.lib_handle = handle;
        api.loaded = true;
        api.load_error.clear();
        return;
      }

      dlclose(handle);
      api.open_fn = nullptr;
      api.close_fn = nullptr;
      api.init_fn = nullptr;
      api.open_handle_fn = nullptr;
      api.get_handle_fn = nullptr;
      api.load_error = "vmem symbols missing in " + std::string(candidate);
    }

    if (api.load_error.empty()) {
      api.load_error = "no usable vmem shared library found";
    }
  });

  return api;
}
#endif

bool get_device_dbdf(ze_device_handle_t device,
                     uint32_t *domain,
                     uint32_t *bus,
                     uint32_t *dev,
                     uint32_t *func) {
  if (domain == nullptr || bus == nullptr || dev == nullptr || func == nullptr) {
    std::fprintf(stderr, "[ZE_PEER_MPI] get_device_dbdf failed: null output pointer\n");
    return false;
  }

  *domain = 0;
  *bus = 0;
  *dev = 0;
  *func = 0;

  auto is_zero_dbdf = [](uint32_t dbdf_domain,
                         uint32_t dbdf_bus,
                         uint32_t dbdf_device,
                         uint32_t dbdf_function) {
    return dbdf_domain == 0 && dbdf_bus == 0 && dbdf_device == 0 &&
           dbdf_function == 0;
  };

#if defined(ZES_STRUCTURE_TYPE_PCI_PROPERTIES)
  static std::once_flag zes_init_once;
  static ze_result_t zes_init_result = ZE_RESULT_SUCCESS;
  std::call_once(zes_init_once, []() { zes_init_result = zesInit(0); });

  if (zes_init_result != ZE_RESULT_SUCCESS) {
    std::fprintf(stderr,
                 "[ZE_PEER_MPI] get_device_dbdf: zesInit ret=%d, fall back to zeDevicePciGetPropertiesExt\n",
                 static_cast<int>(zes_init_result));
  } else {
  zes_pci_properties_t sysman_pci_props = {};
  sysman_pci_props.stype = ZES_STRUCTURE_TYPE_PCI_PROPERTIES;
  const ze_result_t pci_ret =
      zesDevicePciGetProperties(reinterpret_cast<zes_device_handle_t>(device),
                                &sysman_pci_props);
    if (pci_ret == ZE_RESULT_SUCCESS) {
      *domain = sysman_pci_props.address.domain;
      *bus = sysman_pci_props.address.bus;
      *dev = sysman_pci_props.address.device;
      *func = sysman_pci_props.address.function;

      if (!is_zero_dbdf(*domain, *bus, *dev, *func)) {
        return true;
      }

      std::fprintf(stderr,
                   "[ZE_PEER_MPI] get_device_dbdf: zesDevicePciGetProperties returned DBDF 0000:00:00.0, fall back to zeDevicePciGetPropertiesExt\n");
    } else {
      std::fprintf(stderr,
                   "[ZE_PEER_MPI] get_device_dbdf: zesDevicePciGetProperties ret=%d, fall back to zeDevicePciGetPropertiesExt\n",
                   static_cast<int>(pci_ret));
    }
  }
#else
  std::fprintf(stderr,
               "[ZE_PEER_MPI] get_device_dbdf: ZES_STRUCTURE_TYPE_PCI_PROPERTIES is not available, fall back to zeDevicePciGetPropertiesExt\n");
#endif

#if defined(ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES)
  ze_pci_ext_properties_t pci_props = {};
  pci_props.stype = ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES;
  const ze_result_t pci_ext_ret = zeDevicePciGetPropertiesExt(device, &pci_props);
  if (pci_ext_ret != ZE_RESULT_SUCCESS) {
    std::fprintf(stderr,
                 "[ZE_PEER_MPI] get_device_dbdf failed: zeDevicePciGetPropertiesExt ret=%d\n",
                 static_cast<int>(pci_ext_ret));
    return false;
  }

  *domain = pci_props.address.domain;
  *bus = pci_props.address.bus;
  *dev = pci_props.address.device;
  *func = pci_props.address.function;

  if (is_zero_dbdf(*domain, *bus, *dev, *func)) {
    std::fprintf(stderr,
                 "[ZE_PEER_MPI] get_device_dbdf failed: DBDF is 0000:00:00.0\n");
    return false;
  }

  return true;
#else
  std::fprintf(stderr,
               "[ZE_PEER_MPI] get_device_dbdf failed: zeDevicePciGetPropertiesExt is not available\n");
  return false;
#endif
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
  vmem_runtime_api &vmem_api = get_vmem_runtime_api();
  if (!vmem_api.loaded) {
    mpi_abort_with_message("Failed to load vmem runtime API: " + vmem_api.load_error);
  }

  int vmem_fd = vmem_api.open_fn();
  ze_peer_mpi_debug_log(mpi_rank,
                        "vmem_open => fd=%d local_device=%u metadata_tag=%d",
                        vmem_fd,
                        local_device_id,
                        metadata_tag);
  if (vmem_fd < 0) {
    mpi_abort_with_message(std::string("Failed to open /dev/vmem: ") +
                           strerror(errno));
  }

  ze_ipc_mem_handle_t local_handle{};
  int local_dma_buf_fd = -1;
  struct pfn_list local_pfn_list{};
  uint32_t local_domain = 0;
  uint32_t local_bus = 0;
  uint32_t local_device = 0;
  uint32_t local_function = 0;
  bool pfn_ready = false;

  if (!get_device_dbdf(peer->benchmark->_devices[local_device_id],
                       &local_domain,
                       &local_bus,
                       &local_device,
                       &local_function)) {
    vmem_api.close_fn(vmem_fd);
    mpi_abort_with_message("Failed to query PCI DBDF from Level Zero");
  }

  for (int attempt = 0; attempt < mpi_get_pfn_retry_count; attempt++) {
    int open_ret = 0;
    {
      // Serialize handle export + PFN query to avoid cross-thread fd races.
      std::lock_guard<std::mutex> export_lock(mpi_get_pfn_mutex);

      {
        std::lock_guard<std::mutex> ze_lock(ze_ipc_api_mutex);
        peer->benchmark->getIpcHandle(peer->ze_buffers[local_device_id],
                                      &local_handle);
      }

      memcpy(&local_dma_buf_fd, &local_handle, sizeof(local_dma_buf_fd));
      local_pfn_list = {};

      {
        std::lock_guard<std::mutex> ioctl_lock(vmem_ioctl_mutex);
        vmem_api.init_fn(local_domain, local_bus, local_device, local_function);
        ze_peer_mpi_debug_log(mpi_rank,
                              "vmem_open_handle begin: attempt=%d dbdf=%04x:%02x:%02x.%x dma_fd=%d",
                              attempt,
                              local_domain,
                              local_bus,
                              local_device,
                              local_function,
                              local_dma_buf_fd);
        open_ret = vmem_api.open_handle_fn(vmem_fd, &local_handle, &local_pfn_list);
        ze_peer_mpi_debug_log(mpi_rank,
                              "vmem_open_handle end: attempt=%d ret=%d "
                              "errno=%d nents=%d",
                              attempt,
                              open_ret,
                              errno,
                              local_pfn_list.nents);
      }

      {
        std::lock_guard<std::mutex> ze_lock(ze_ipc_api_mutex);
        peer->benchmark->putIpcHandle(local_handle);
      }
    }

    if (open_ret == 0) {
      if (local_pfn_list.nents <= 0) {
        vmem_api.close_fn(vmem_fd);
        mpi_abort_with_message("Local PFN list is empty");
      }
      pfn_ready = true;
      break;
    }

    const int ioctl_errno = errno;
    if (ioctl_errno == EBADF && (attempt + 1) < mpi_get_pfn_retry_count) {
      continue;
    }

    vmem_api.close_fn(vmem_fd);
    mpi_abort_with_message(std::string("vmem_open_handle failed: ") +
                           strerror(ioctl_errno));
  }

  if (!pfn_ready) {
    vmem_api.close_fn(vmem_fd);
    mpi_abort_with_message("vmem_open_handle failed after retries");
  }

  mpi_exchange_data send_data{};
  send_data.dma_buf_fd = local_dma_buf_fd;
  memcpy(&send_data.pfn_list, &local_pfn_list, sizeof(send_data.pfn_list));

  mpi_exchange_data recv_data{};
  int peer_rank = 1 - mpi_rank;
  ze_peer_mpi_debug_log(mpi_rank,
                        "metadata exchange begin: peer_rank=%d tag=%d local_dma_fd=%d pfn_nents=%d",
                        peer_rank,
                        metadata_tag,
                        send_data.dma_buf_fd,
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
                        "metadata exchange end: ret=%d remote_dma_fd=%d remote_pfn_nents=%d",
                        mpi_ret,
                        recv_data.dma_buf_fd,
                        recv_data.pfn_list.nents);
  if (mpi_ret != MPI_SUCCESS) {
    vmem_api.close_fn(vmem_fd);
    mpi_abort_with_message("MPI_Sendrecv failed while exchanging IPC metadata");
  }

  struct pfn_list remote_pfn_list{};
  int remote_dma_buf_fd = -1;
  memcpy(&remote_pfn_list, &recv_data.pfn_list, sizeof(remote_pfn_list));

  int import_ret = 0;
  {
    std::lock_guard<std::mutex> lock(vmem_ioctl_mutex);
    ze_peer_mpi_debug_log(mpi_rank,
                          "vmem_get_handle begin: remote_pfn_nents=%d",
                          remote_pfn_list.nents);
    import_ret = vmem_api.get_handle_fn(vmem_fd, &remote_dma_buf_fd, &remote_pfn_list);
    ze_peer_mpi_debug_log(mpi_rank,
                          "vmem_get_handle end: ret=%d errno=%d new_fd=%d",
                          import_ret,
                          errno,
                          remote_dma_buf_fd);
  }
  if (import_ret < 0) {
    const int open_errno = errno;
    vmem_api.close_fn(vmem_fd);
    mpi_abort_with_message(std::string("vmem_get_handle failed: ") +
                           strerror(open_errno));
  }

  if (remote_dma_buf_fd < 0) {
    vmem_api.close_fn(vmem_fd);
    mpi_abort_with_message("vmem_get_handle returned invalid dma-buf fd");
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

  vmem_api.close_fn(vmem_fd);
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