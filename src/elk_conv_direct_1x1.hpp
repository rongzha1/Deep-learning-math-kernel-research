#ifndef __ELK_CONV_DIRECT_1X1_HPP__
#define __ELK_CONV_DIRECT_1X1_HPP__

#include <boost/preprocessor/seq/for_each_product.hpp>
#include <boost/preprocessor/seq/to_tuple.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/elem.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elx_conv.hpp"

namespace euler {

// Type: data type
// S: stride
// O2: OC blocking
// T: tile blocking unit
// V: vector size
// I: ISA
// with_bias: has bias
// with_relu: with relu fusion
// with_sum: with sum fusion

#define BIAS(x) x
#define BORDER(x) x
#define RELU(x) x
#define SUM(x) x

// Kernel data format:
// input, weights, output
// C: compact
// S: scatter
const int CCC = 1;
const int CCS = 2;
const int SSS = 3;

template <typename Type, const int S, const int O2, const int T, const int fmt,
    const int V, const int I, const bool with_bias, const bool with_relu,
    const bool with_sum>
struct convolution_direct_1x1_kernel {
  static void gemm(elx_conv_t<Type> &xc, Type *output, Type *input,
      Type *weights, Type *bias, bool reset_out = false);
  static void gemm_tail(elx_conv_t<Type> &xc, Type *output, Type *input,
      Type *weights, Type *bias, bool reset_out = false);
};

#define DEF_convolution_direct_1x1_kernel(O, T)                                \
  template <typename Type, const int S, const int V, const int I,              \
      const bool with_bias, const bool with_relu, const bool with_sum>         \
  struct convolution_direct_1x1_kernel<Type, S, O, T, SSS, V, I, with_bias,    \
      with_relu, with_sum> {                                                   \
    static void gemm(elx_conv_t<Type> &xc, Type *output, Type *input,          \
        Type *weights, Type *bias, bool reset_out = false);                    \
    static void gemm_tail(elx_conv_t<Type> &xc, Type *output, Type *input,     \
        Type *weights, Type *bias, bool reset_out = false);                    \
  };                                                                           \
  template <typename Type, const int V, const int I, const bool with_bias,     \
      const bool with_relu, const bool with_sum>                               \
  struct convolution_direct_1x1_kernel<Type, 1, O, T, SSS, V, I, with_bias,    \
      with_relu, with_sum> {                                                   \
    static void gemm(elx_conv_t<Type> &xc, Type *output, Type *input,          \
        Type *weights, Type *bias, bool reset_out = false);                    \
    static void gemm_tail(elx_conv_t<Type> &xc, Type *output, Type *input,     \
        Type *weights, Type *bias, bool reset_out = false);                    \
  };                                                                           \
  template <typename Type, const int V, const int I, const bool with_bias,     \
      const bool with_relu, const bool with_sum>                               \
  struct convolution_direct_1x1_kernel<Type, 1, O, T, CCS, V, I, with_bias,    \
      with_relu, with_sum> {                                                   \
    static void gemm(elx_conv_t<Type> &xc, Type *output, Type *input,          \
        Type *weights, Type *bias, bool reset_out = false);                    \
    static void gemm_tail(elx_conv_t<Type> &xc, Type *output, Type *input,     \
        Type *weights, Type *bias, bool reset_out = false);                    \
  };                                                                           \
  template <typename Type, const int V, const int I, const bool with_bias,     \
      const bool with_relu, const bool with_sum>                               \
  struct convolution_direct_1x1_kernel<Type, 1, O, T, CCC, V, I, with_bias,    \
      with_relu, with_sum> {                                                   \
    static void gemm(elx_conv_t<Type> &xc, Type *output, Type *input,          \
        Type *weights, Type *bias, bool reset_out = false);                    \
    static void gemm_tail(elx_conv_t<Type> &xc, Type *output, Type *input,     \
        Type *weights, Type *bias, bool reset_out = false);                    \
  };

DEF_convolution_direct_1x1_kernel(1, 1);
DEF_convolution_direct_1x1_kernel(1, 2);
DEF_convolution_direct_1x1_kernel(1, 3);
DEF_convolution_direct_1x1_kernel(1, 4);
DEF_convolution_direct_1x1_kernel(1, 5);
DEF_convolution_direct_1x1_kernel(1, 6);
DEF_convolution_direct_1x1_kernel(1, 7);
DEF_convolution_direct_1x1_kernel(1, 8);
DEF_convolution_direct_1x1_kernel(1, 9);
DEF_convolution_direct_1x1_kernel(1, 10);
DEF_convolution_direct_1x1_kernel(1, 11);
DEF_convolution_direct_1x1_kernel(1, 12);
DEF_convolution_direct_1x1_kernel(1, 13);
DEF_convolution_direct_1x1_kernel(1, 14);
DEF_convolution_direct_1x1_kernel(1, 15);
DEF_convolution_direct_1x1_kernel(1, 16);
DEF_convolution_direct_1x1_kernel(1, 17);
DEF_convolution_direct_1x1_kernel(1, 18);
DEF_convolution_direct_1x1_kernel(1, 19);
DEF_convolution_direct_1x1_kernel(1, 20);
DEF_convolution_direct_1x1_kernel(1, 21);
DEF_convolution_direct_1x1_kernel(1, 22);
DEF_convolution_direct_1x1_kernel(1, 23);
DEF_convolution_direct_1x1_kernel(1, 24);
DEF_convolution_direct_1x1_kernel(1, 25);
DEF_convolution_direct_1x1_kernel(1, 26);
DEF_convolution_direct_1x1_kernel(1, 27);
DEF_convolution_direct_1x1_kernel(1, 28);
DEF_convolution_direct_1x1_kernel(1, 29);
DEF_convolution_direct_1x1_kernel(1, 30);
DEF_convolution_direct_1x1_kernel(1, 31);
DEF_convolution_direct_1x1_kernel(1, 32);
DEF_convolution_direct_1x1_kernel(1, 33);
DEF_convolution_direct_1x1_kernel(1, 34);
DEF_convolution_direct_1x1_kernel(1, 35);
DEF_convolution_direct_1x1_kernel(2, 1);
DEF_convolution_direct_1x1_kernel(2, 2);
DEF_convolution_direct_1x1_kernel(2, 3);
DEF_convolution_direct_1x1_kernel(2, 4);
DEF_convolution_direct_1x1_kernel(2, 5);
DEF_convolution_direct_1x1_kernel(2, 6);
DEF_convolution_direct_1x1_kernel(2, 7);
DEF_convolution_direct_1x1_kernel(2, 8);
DEF_convolution_direct_1x1_kernel(2, 9);
DEF_convolution_direct_1x1_kernel(2, 10);
DEF_convolution_direct_1x1_kernel(2, 11);
DEF_convolution_direct_1x1_kernel(2, 12);
DEF_convolution_direct_1x1_kernel(2, 13);
DEF_convolution_direct_1x1_kernel(2, 14);
DEF_convolution_direct_1x1_kernel(3, 1);
DEF_convolution_direct_1x1_kernel(3, 2);
DEF_convolution_direct_1x1_kernel(3, 3);
DEF_convolution_direct_1x1_kernel(3, 4);
DEF_convolution_direct_1x1_kernel(3, 5);
DEF_convolution_direct_1x1_kernel(3, 6);
DEF_convolution_direct_1x1_kernel(3, 7);
DEF_convolution_direct_1x1_kernel(3, 8);
DEF_convolution_direct_1x1_kernel(3, 9);
DEF_convolution_direct_1x1_kernel(3, 10);
DEF_convolution_direct_1x1_kernel(3, 11);
DEF_convolution_direct_1x1_kernel(3, 12);
DEF_convolution_direct_1x1_kernel(3, 13);
DEF_convolution_direct_1x1_kernel(3, 14);
DEF_convolution_direct_1x1_kernel(4, 1);
DEF_convolution_direct_1x1_kernel(4, 2);
DEF_convolution_direct_1x1_kernel(4, 3);
DEF_convolution_direct_1x1_kernel(4, 4);
DEF_convolution_direct_1x1_kernel(4, 5);
DEF_convolution_direct_1x1_kernel(4, 6);
DEF_convolution_direct_1x1_kernel(4, 7);
DEF_convolution_direct_1x1_kernel(4, 8);
DEF_convolution_direct_1x1_kernel(4, 9);
DEF_convolution_direct_1x1_kernel(4, 10);
DEF_convolution_direct_1x1_kernel(4, 11);
DEF_convolution_direct_1x1_kernel(4, 12);
DEF_convolution_direct_1x1_kernel(4, 13);
DEF_convolution_direct_1x1_kernel(4, 14);
DEF_convolution_direct_1x1_kernel(5, 1);
DEF_convolution_direct_1x1_kernel(5, 2);
DEF_convolution_direct_1x1_kernel(5, 3);
DEF_convolution_direct_1x1_kernel(5, 4);
DEF_convolution_direct_1x1_kernel(5, 5);
DEF_convolution_direct_1x1_kernel(6, 1);
DEF_convolution_direct_1x1_kernel(6, 2);
DEF_convolution_direct_1x1_kernel(6, 3);
DEF_convolution_direct_1x1_kernel(6, 4);
DEF_convolution_direct_1x1_kernel(7, 1);
DEF_convolution_direct_1x1_kernel(7, 2);
DEF_convolution_direct_1x1_kernel(7, 3);
DEF_convolution_direct_1x1_kernel(8, 1);
DEF_convolution_direct_1x1_kernel(8, 2);
DEF_convolution_direct_1x1_kernel(8, 3);
DEF_convolution_direct_1x1_kernel(8, 4);
DEF_convolution_direct_1x1_kernel(8, 5);
DEF_convolution_direct_1x1_kernel(8, 6);
DEF_convolution_direct_1x1_kernel(8, 7);
DEF_convolution_direct_1x1_kernel(8, 8);

} // namespace euler


#endif // __ELK_CONV_DIRECT_1X1_HPP__