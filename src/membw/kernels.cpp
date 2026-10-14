// The four STREAM kernels, in their own file so they can be compiled without
// loop-idiom recognition: the copy loop must stay a loop, not become a
// memcpy call that may switch to non-temporal stores (see CMakeLists.txt).
#include "membw/stream.hpp"

namespace hwdiag::membw {

void run_kernel(Kernel k, double* __restrict a, double* __restrict b, double* __restrict c, size_t begin,
                size_t end) {
  switch (k) {
    case Kernel::Copy:
      for (size_t j = begin; j < end; ++j) c[j] = a[j];
      break;
    case Kernel::Scale:
      for (size_t j = begin; j < end; ++j) b[j] = kScalar * c[j];
      break;
    case Kernel::Add:
      for (size_t j = begin; j < end; ++j) c[j] = a[j] + b[j];
      break;
    case Kernel::Triad:
      for (size_t j = begin; j < end; ++j) a[j] = b[j] + kScalar * c[j];
      break;
  }
}

}  // namespace hwdiag::membw
