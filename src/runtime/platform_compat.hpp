#pragma once
// What the shared code needs from the host that Apple and Linux spell
// differently. On Apple this is the system's own (TargetConditionals, thread
// QoS classes, Mach); elsewhere, stand-ins that do the same or nothing:
//   TARGET_OS_IPHONE / TARGET_OS_OSX   0 off Apple
//   thread QoS classes                 accepted and ignored off Apple
//   host_page_readable(p, n, out)      copies n bytes at p without faulting
//                                      (false where they are not mapped)
//   MADV_FREE_REUSABLE / MADV_FREE_REUSE  MADV_FREE and nothing off Apple
//   std::bit_cast                      __builtin_bit_cast where the C++
//                                      library predates it (libstdc++ 10, the
//                                      Steam Runtime's)
#include <bit>
#include <cstddef>
#include <cstdint>
#include <pthread.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#ifndef MADV_FREE_REUSABLE
#define MADV_FREE_REUSABLE MADV_FREE  // pages back to the system, contents undefined
#endif
#ifndef MADV_FREE_REUSE
#define MADV_FREE_REUSE MADV_NORMAL  // nothing to undo on Linux
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#endif
using qos_class_t = int;
constexpr qos_class_t QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19, QOS_CLASS_DEFAULT = 0x15,
                      QOS_CLASS_UTILITY = 0x11, QOS_CLASS_BACKGROUND = 0x09;
inline int pthread_set_qos_class_self_np(qos_class_t, int) { return 0; }
inline int pthread_attr_set_qos_class_np(pthread_attr_t*, qos_class_t, int) { return 0; }
#endif

#if !defined(__cpp_lib_bit_cast)
namespace std {
template <class To, class From>
constexpr To bit_cast(const From& from) noexcept {
    return __builtin_bit_cast(To, from);
}
}  // namespace std
#endif

namespace dcusa {

// Copies `size` bytes at `at` into `out`; false (and no fault) when they are
// not all readable.
inline bool host_page_readable(const void* at, std::size_t size, void* out) {
#if defined(__APPLE__)
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), reinterpret_cast<vm_address_t>(at), size,
                             reinterpret_cast<vm_address_t>(out), &got) == KERN_SUCCESS &&
           got == size;
#else
    iovec local{out, size}, remote{const_cast<void*>(at), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
#endif
}

}  // namespace dcusa
