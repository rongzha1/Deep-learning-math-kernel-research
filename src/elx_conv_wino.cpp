#include <string.h>
#include <x86intrin.h>
#include "el_utils.hpp"
#include "elx_conv_wino.hpp"
#include "el_def.hpp"
#include "el_utils.hpp"
#include "elk_conv_wino.hpp"
#include "elx_conv.hpp"
#include "euler.hpp"

namespace euler {

//
// -------------+-------------------+--------------+---------------
//  execute-opt | thread-teaming-by | fusion-along | duplication
// -------------+-------------------+--------------+---------------
//     A040     |        _          |      t       |    _
// -------------+-------------------+--------------+---------------
//     A048*    |        _          |      t       |    W
// -------------+-------------------+--------------+---------------
//     A060*    |        _          |    t + o     |    _
// -------------+-------------------+--------------+---------------
//     A061     |        _          |    t + o     |    I
// -------------+-------------------+--------------+---------------
//     A069*    |        _          |    t + o     |  I + W
// -------------+-------------------+--------------+---------------
//     A0e1     |        _          |  t + o + wA  |    I
// -------------+-------------------+--------------+---------------
//     A0e0     |        _          |  t + o + wA  |    _
// -------------+-------------------+--------------+---------------
//     A073     |        _          |  t + o + i   |  I + O
// -------------+-------------------+--------------+---------------
//     A448     |        t          |      t       |    W
// -------------+-------------------+--------------+---------------
//     A241     |        o          |      t       |    I
// -------------+-------------------+--------------+---------------
//     A000     |        _          |      _       |    _
// -------------+-------------------+--------------+---------------
//     A201     |        o          |      _       |    I
// -------------+-------------------+--------------+---------------
//     A020*    |        _          |      o       |    _
// -------------+-------------------+--------------+---------------
//     A021*    |        _          |      o       |    I
// -------------+-------------------+--------------+---------------
//  *: TODO
//

const unsigned XOPT_MSK = 0xA000;

const unsigned TTM_MSK = 0xF00;
const unsigned TTM_I   = 0x100;
const unsigned TTM_O   = 0x200;
const unsigned TTM_T   = 0x400;

const unsigned FUS_MSK = 0xF0;
const unsigned FUS_I   = 0x10;
const unsigned FUS_O   = 0x20;
const unsigned FUS_T   = 0x40;
const unsigned FUS_A   = 0x80;

const unsigned DUP_MSK = 0xF;
const unsigned DUP_I   = 0x1;
const unsigned DUP_O   = 0x2;
const unsigned DUP_W   = 0x8;

template <typename Type, const int A, const int K, const int V, const int I>
elx_conv_wino_t<Type, A, K, V, I>::elx_conv_wino_t(
    eld_conv_t<Type>& dc)
    : elx_conv_t<Type>(dc)
{
  this->V = V;
  this->ic2 = this->ic / V;
  this->oc2 = this->oc / V;

  this->A = A;
  this->ht = (this->oh + A - K) / (A - K + 1);
  this->wt = (this->ow + A - K) / (A - K + 1);
  this->nt = this->ht * this->wt;
  this->t = this->nt * this->n;

  hOA_end_ = this->oh % (A - K + 1) - 1;
  if (hOA_end_ == -1) hOA_end_ = A - K;
  wOA_end_ = this->ow % (A - K + 1) - 1;
  if (wOA_end_ == -1) wOA_end_ = A - K;
  hA_end_ = (this->ih + this->tp) - (this->ht - 1) * (A - K + 1) - 1;
  wA_end_ = (this->iw + this->lp) - (this->wt - 1) * (A - K + 1) - 1;

  // trans-buffer blocking
  // ic: ic3, I2, V
  // oc: oc3, O2, V
  // t : t2,  T
  // I2, O2
  // tweights + pt-tinputs + pt-toutput ~ L2
  // tweights:gemm + tinputs:gemm + toutput:gemm ~ L1

  // TODO: santize user settings
  if (this->I2 == 0) this->I2 = 4; // TODO: I2 selection
  if (this->O2 == 0) this->O2 = 2; // TODO: O2 selection
  if (this->T == 0)  this->T = 18; // TODO: T selection

  // Tailing
  this->Ir = this->ic % V;
  this->Or = this->oc % V;
  this->Tr = this->t % this->T;
  if (this->Tr == 0) this->Tr = this->T;
  // TODO: support tailing
  assert(this->Ir == 0);
  assert(this->Or == 0);

  is_first_run_ = true;
  inference_acc_ = false;
  mthr_ = omp_get_max_threads();
  if (this->nteams == 0 || this->nthreads == 0
      || this->nteams * this->nthreads > mthr_
      || this->nteams > MAX_THREAD_TEAMS) {
    this->nteams = 1;
    this->nthreads = mthr_;
  } else {
    mthr_ = this->nteams * this->nthreads;
  }
  inference_acc_ = this->prop_kind == forward_inference;

  // TODO: add tailing?
  this->oc4 = this->oc4 == 0 ? 1 : this->oc4;
  this->ic4 = this->ic4 == 0 ? 1 : this->ic4;
  this->oc3 = this->oc / (this->O2 * V);
  this->ic3 = this->ic / (this->I2 * V);
  this->t2 = (this->t + this->T - 1) / this->T;

  xopt_ = this->execution_mode;
  if (!(xopt_& XOPT_MSK)) {
    // TODO: deduce xopt
    xopt_ = TTM_O | FUS_T | DUP_I;
  }

  prepare_execute_opt();
  bind_execute_functions();

  // dbg
  printf("T=%d, Tr=%d, t2=%d, t=%d\n", this->T, this->Tr, this->t2, this->t);
  printf("V=%d, Ir=%d, I2=%d, ic3=%d, ic4=%d, ic=%d\n", this->V, this->Ir, this->I2, this->ic3, this->ic4, this->ic);
  printf("V=%d, Or=%d, O2=%d, oc3=%d, oc4=%d, oc=%d\n", this->V, this->Or, this->O2, this->oc3, this->oc4, this->oc);
}

template <typename Type, const int A, const int K, const int V, const int I>
int  elx_conv_wino_t<Type, A, K, V, I>::prepare_execute_opt()
{
  size_t tweights_size = 0, tinput_size = 0, toutput_size = 0;
  size_t routput_size = 0, toutputa_size = 0;
  size_t binput_size = 0, bweights_size = 0, boutput_size = 0;
  size_t l1_usage = 0, l2_usage = 0;

  auto divide_tasks_ttm = [this](size_t tasks) {
    size_t ntasks_base = tasks / this->nteams;
    size_t rem = tasks - this->nteams * ntasks_base;
    for (size_t s = 0; s < this->nteams; s++) {
      if (s < rem) {
        ttm_[s].start = (ntasks_base + 1) * s;
        ttm_[s].end = ttm_[s].start + ntasks_base;
      } else {
        ttm_[s].start = rem * (ntasks_base + 1) + (s - rem) * ntasks_base;
        ttm_[s].end = ttm_[s].start + ntasks_base - 1;
      }
      // dbg
      printf("ttm_[%ld]=[%d,%d]\n", s, ttm_[s].start, ttm_[s].end);
    }
  };

  stream_in_ = this->streaming_input
      ? (this->streaming_input == STORE_STREAMING)
      : !(xopt_ & FUS_MSK) ? true : false;
  stream_wei_ = this->streaming_weights
      ? (this->streaming_weights == STORE_STREAMING)
      : !(xopt_ & FUS_MSK) ? true : false;
  stream_out_ = this->streaming_output
      ? (this->streaming_output == STORE_STREAMING)
      : false;

  if (!(xopt_ & TTM_MSK)) {
    this->nthreads = mthr_;
    this->nteams = 1;
  }
  if (xopt_ & TTM_T) {
    divide_tasks_ttm(this->t2);
  }
  if (xopt_ & TTM_O) {
    if (this->oc3 % this->nteams != 0) {
      // Force single nteams
      this->nthreads = mthr_;
      this->nteams = 1;
    } else {
      // ignore user --pat-o=oc4
      this->oc3 /= this->nteams;
      this->oc4 = this->nteams;
    }
  }
  if (xopt_ & FUS_O) {
    this->oc3 /= this->oc4;
    if (V * this->O2 * this->oc3 * this->oc4 != this->oc) {
      el_error("Config error!");
      return -1;
    }
  }
  if (xopt_ & FUS_I) {
    this->ic3 /= this->ic4;
    if (V * this->I2 * this->ic3 * this->ic4 != this->ic) {
      el_error("Config error!");
      return -1;
    }
  }

  input_is_bfmt_ = this->input_fmt == nchw ? false : true;
  weights_is_bfmt_ = this->weights_fmt == oihw ? false : true;
  output_is_bfmt_ = this->output_fmt == nchw ? false : true;
  input_as_bfmt_ = !input_is_bfmt_ && this->input_as_blocked;
  weights_as_bfmt_ = !weights_is_bfmt_ && this->weights_as_blocked;
  output_as_bfmt_ = !output_is_bfmt_ && this->output_as_blocked;
  is_bfmt_ = input_is_bfmt_ && weights_is_bfmt_ && output_is_bfmt_;

  if (input_as_bfmt_)
    binput_size = this->n * this->ic * this->ih * this->iw;
  if (weights_as_bfmt_)
    bweights_size = this->oc * this->ic * this->kh * this->kw;
  if (output_as_bfmt_)
    boutput_size = this->n * this->oc * this->oh * this->ow;

  tweights_ = nullptr;
  tinput_ = nullptr;
  toutput_ = nullptr;
  toutputa_ = nullptr;
  routput_ = nullptr;
  routput_cntr_ = nullptr;
  binput_ = nullptr;
  bweights_ = nullptr;
  boutput_ = nullptr;

  l1_usage = sizeof(Type)
      * (this->O2 * this->I2 * V * V + this->T * V * (this->I2 + this->O2));
MEMALIGN64(&boutput_, this->n * this->oc * this->oh * this->ow);

  switch (xopt_) {
  case 0xa000:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * this->ic * this->t;
    toutput_size = A * A * this->oc * this->t;
    l2_usage = this->ic * this->oc / this->oc3
        + this->T * (this->ic + this->oc / this->oc3);
    break;
  case 0xa040:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * this->ic * this->T * mthr_;
    toutput_size = A * A * this->oc * this->T * mthr_;
    l2_usage = tweights_size + A * A * this->T * (this->ic + this->oc);
    break;
  case 0xa061:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * this->ic * this->T * mthr_;
    toutput_size = A * A * (this->oc / this->oc4) * this->T * mthr_;
    l2_usage = tweights_size / this->oc4
        + A * A * this->T * (this->ic + this->oc / this->oc4);
    break;
  case 0xa073:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * (this->ic / this->ic4) * this->T * mthr_;
    toutput_size = A * A * (this->oc / this->oc4) * this->T * mthr_;
    routput_size = this->n * this->oc * this->oh * this->ow * (this->ic4 - 1);
    routput_cntr_ = (unsigned char *)malloc(this->t2 * this->oc4);
    l2_usage = tweights_size / this->ic4 / this->oc4
        + A * A * this->T * (this->ic / this->ic4 + this->oc / this->oc4);
    break;
  case 0xa0e0:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * this->ic * this->t;
    toutput_size = A * (this->oc / this->oc4) * this->T * mthr_;
    toutputa_size = A * (A - K + 1) * this->oc * this->t;
    l2_usage = tweights_size / this->oc4 / A
        + A * this->T * (this->ic + this->oc / this->oc4);
    break;
  case 0xa0e1:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * this->ic * this->T * mthr_;
    toutput_size = A * (this->oc / this->oc4) * this->T * mthr_;
    toutputa_size = A * (A - K + 1) * this->oc * this->t;
    l2_usage = tweights_size / this->oc4 / A
        + A * this->T * (this->ic + this->oc / this->oc4);
    break;
  case 0xa201:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * this->ic * this->T * this->t2 * this->nteams;
    toutput_size = A * A * this->oc * this->T * this->t2;
    l2_usage = this->ic * this->oc / this->oc3 / this->oc4
        + this->T * (this->ic + this->oc / this->oc3 / this->oc4);
    break;
  case 0xa241:
    tweights_size = A * A * this->ic * this->oc;
    tinput_size = A * A * this->ic * this->T * mthr_;
    toutput_size = A * A * this->oc * this->T * mthr_;
    l2_usage = tweights_size / this->oc4
        + A * A * this->T * (this->ic + this->oc / this->oc4);
    break;
  case 0xa448:
    tweights_size = A * A * this->ic * this->oc * this->nteams;
    tinput_size = A * A * this->ic * this->T * mthr_;
    toutput_size = A * A * this->oc * this->T * mthr_;
    l2_usage = tweights_size / this->nteams
        + A * A * this->T * (this->ic + this->oc);
    break;
  default:
      el_error("Config error!");
      return -1;
    break;
  }

  l2_usage *= sizeof(Type);

  if (tweights_size > 0)
    MEMALIGN64(&tweights_, tweights_size * sizeof(Type));
  if (tinput_size > 0)
    MEMALIGN64(&tinput_, tinput_size * sizeof(Type));
  if (toutput_size > 0)
    MEMALIGN64(&toutput_, toutput_size * sizeof(Type));
  if (routput_size > 0)
    MEMALIGN64(&routput_, routput_size * sizeof(Type));
  if (toutputa_size > 0)
    MEMALIGN64(&toutputa_, toutputa_size * sizeof(Type));
  if (binput_size > 0)
    MEMALIGN64(&binput_, binput_size * sizeof(Type));
  if (bweights_size > 0)
    MEMALIGN64(&bweights_, bweights_size * sizeof(Type));
  if (boutput_size > 0)
    MEMALIGN64(&boutput_, boutput_size * sizeof(Type));

  // dbg
  printf("nteams=%d, nthreads=%d, mthr_=%ld\n", this->nteams, this->nthreads, mthr_);
  printf("l2_usage=%ld, l1_usage=%ld\n", l2_usage, l1_usage);

  return 0;
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::bind_execute_functions()
{
  ker_trans_input_ = convolution_winograd_kernel<S_INPUT(
      Type, A, K, V, I, BORDER(false))>::trans_input;
  ker_trans_input0_ = convolution_winograd_kernel<S_INPUT(
      Type, A, K, V, I, BORDER(true))>::trans_input;
  ker_trans_inputa_ = convolution_winograd_kernel<S_INPUT(
      Type, A, K, V, I, BORDER(false))>::trans_inputa;
  ker_trans_inputa0_ = convolution_winograd_kernel<S_INPUT(
      Type, A, K, V, I, BORDER(true))>::trans_inputa;
  ker_trans_weights_ = convolution_winograd_kernel<S_WEIGHTS(
      Type, A, K, V, I)>::trans_weights;
  ker_trans_output_nobias_ = convolution_winograd_kernel<S_OUTPUT(
      Type, A, K, V, I, BORDER(false), BIAS(false))>::trans_output;
  ker_trans_output0_nobias_ = convolution_winograd_kernel<S_OUTPUT(
      Type, A, K, V, I, BORDER(true), BIAS(false))>::trans_output;
  if (this->with_bias) {
    ker_trans_output_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(false), BIAS(true))>::trans_output;
    ker_trans_output0_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(true), BIAS(true))>::trans_output;
    ker_trans_outputa_bh_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(false), BIAS(true))>::trans_outputa_bh;
    ker_trans_outputa0_bh_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(true), BIAS(true))>::trans_outputa_bh;
  } else {
    ker_trans_output_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(false), BIAS(false))>::trans_output;
    ker_trans_output0_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(true), BIAS(false))>::trans_output;
    ker_trans_outputa_bh_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(false), BIAS(false))>::trans_outputa_bh;
    ker_trans_outputa0_bh_ = convolution_winograd_kernel<S_OUTPUT(
        Type, A, K, V, I, BORDER(true), BIAS(false))>::trans_outputa_bh;
  }
  ker_trans_outputa_th_ = convolution_winograd_kernel<S_OUTPUT(
      Type, A, K, V, I, BORDER(false), BIAS(false))>::trans_outputa_th;

#define GEMM_CASE(z, n, data)                                                \
  case n:                                                                    \
    ker_gemm_ = convolution_winograd_kernel<S_GEMM(Type, n, V, I)>::gemm;    \
    break;

  switch (this->T) {
    BOOST_PP_REPEAT_FROM_TO(1, MAX_FMA_PRL, GEMM_CASE, nil)
  default:
    el_error("Unimplemented");
    break;
  }

#define GEMM_CASE0(z, n, data)                                               \
  case n:                                                                    \
    ker_gemm0_ = convolution_winograd_kernel<S_GEMM(Type, n, V, I)>::gemm;   \
    break;

  switch (this->Tr) {
    BOOST_PP_REPEAT_FROM_TO(1, MAX_FMA_PRL, GEMM_CASE0, nil);
  default:
    el_error("Unimplemented");
    break;
  }

#define EXECUTE_CASE(n)                                                      \
  case 0x##n:                                                                \
    printf("execute_opt=" #n "\n");\
    execute_opt_ = &elx_conv_wino_t<Type, A, K, V, I>::__execute_##n;   \
    break

  switch (xopt_) {
  EXECUTE_CASE(a241);
  EXECUTE_CASE(a201);
  EXECUTE_CASE(a448);
  EXECUTE_CASE(a040);
  EXECUTE_CASE(a061);
  EXECUTE_CASE(a0e1);
  EXECUTE_CASE(a0e0);
  EXECUTE_CASE(a073);
  EXECUTE_CASE(a000);
  default:
    el_error("Unimplemented");
    break;
  }
}

template <typename Type, const int A, const int K, const int V, const int I>
elx_conv_wino_t<Type, A, K, V, I>::~elx_conv_wino_t()
{
  if (tweights_ != nullptr) {
    free(tweights_);
    tweights_ = nullptr;
  }
  if (tinput_ != nullptr) {
    free(tinput_);
    tinput_ = nullptr;
  }
  if (toutput_ != nullptr) {
    free(toutput_);
    toutput_ = nullptr;
  }
  if (routput_ != nullptr) {
    free(routput_);
    routput_ = nullptr;
  }
  if (routput_cntr_ != nullptr) {
    free(routput_cntr_);
    routput_cntr_ = nullptr;
  }
  if (toutputa_ != nullptr) {
    free(toutputa_);
    toutputa_ = nullptr;
  }
  if (binput_ != nullptr) {
    free(binput_);
    binput_ = nullptr;
  }
  if (bweights_ != nullptr) {
    free(bweights_);
    bweights_ = nullptr;
  }
  if (boutput_ != nullptr) {
    free(boutput_);
    boutput_ = nullptr;
  }
}

#define t2spato(__t2, __T, __n, __oh, __ow, __hOA_end, __wOA_end)            \
  do {                                                                       \
    int _t = __t2 * this->T + __T;                                           \
    int _nt = _t % this->nt;                                                 \
    int _ht = _nt / this->wt;                                                \
    int _wt = _nt % this->wt;                                                \
    __n = _t / this->nt;                                                     \
    __oh = _ht * (A - K + 1);                                                \
    __ow = _wt * (A - K + 1);                                                \
    __hOA_end = (_ht < this->ht - 1) ? A - K : hOA_end_;                     \
    __wOA_end = (_wt < this->wt - 1) ? A - K : wOA_end_;                     \
  } while (0)

#define t2spati(                                                             \
    __t2, __T, __n, __ih, __iw, __hA_start, __hA_end, __wA_start, __wA_end)  \
  do {                                                                       \
    int _t = __t2 * this->T + __T;                                           \
    int _nt = _t % this->nt;                                                 \
    int _ht = _nt / this->wt;                                                \
    int _wt = _nt % this->wt;                                                \
    __n = _t / this->nt;                                                     \
    __ih = _ht * (A - K + 1) - this->tp;                                     \
    __iw = _wt * (A - K + 1) - this->lp;                                     \
    __hA_start = (_ht > 0) ? 0 : this->tp;                                   \
    __wA_start = (_wt > 0) ? 0 : this->lp;                                   \
    __hA_end = (_ht < this->ht - 1) ? A - 1 : hA_end_;                       \
    __wA_end = (_wt < this->wt - 1) ? A - 1 : wA_end_;                       \
  } while (0)

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weights_plain(
    Type * __restrict tweights, Type * __restrict weights, int oc4)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, oc3, ic3, wA, hA, O2, I2, V, V
  MD(Type, aweights, [oc4][this->oc3][this->O2][V][this->ic4][this->ic3][this->I2][V][K][K], weights);
  MD(Type, atweights, [oc4][this->ic4][this->oc3][this->ic3][A][A][this->O2][this->I2][V][V], tweights);

  int s = this->ic * this->kh * this->kw;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto readin = [&](Type ain[K][K][V][V], Type *wei) {
    MD(Type, awei, [V][this->ic2][V][K][K], wei);

    for_each (_hK, K) {
    for_each (_wK, K) {
    for_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        constexpr auto scale = sizeof(Type);
        auto t = _mm512_i32gather_ps(vindex,
            (float *)&awei[0][0][_iV][_hK][_wK], scale);
        _mm512_store_ps(ain[_hK][_wK][_iV], t);
      } else {
        for_each (_oV, V)
          ain[_hK][_wK][_iV][_oV] = awei[_oV][0][_iV][_hK][_wK];
      }
    }}}
  };

#pragma omp for nowait collapse(6) schedule(static)
  for_each (_oc4, oc4) {
  for_each (_ic4, this->ic4) {
  for_each (_oc3, this->oc3) {
  for_each (_ic3, this->ic3) {
  for_each (_O2, this->O2) {
  for_each (_I2, this->I2) {

    alignas(64) Type ain[K][K][V][V];
    alignas(64) Type aout[A][A][V][V];

    readin(ain, (Type *)aweights[_oc4][_oc3][_O2][0][_ic4][_ic3][_I2]);

    ker_trans_weights_(aout, ain);
    for_each (_wA, A) {
    for_each (_hA, A) {
    for_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_wei_)
          _mm512_stream_ps(
              atweights[_oc4][_ic4][_oc3][_ic3][_wA][_hA][_O2][_I2][_iV],
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        else
          _mm512_store_ps(
              atweights[_oc4][_ic4][_oc3][_ic3][_wA][_hA][_O2][_I2][_iV],
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
      } else {
#pragma omp simd
        for_each (_oV, V)
          atweights[_oc4][_ic4][_oc3][_ic3][_wA][_hA][_O2][_I2][_iV][_oV]
              = aout[_wA][_hA][_iV][_oV];
      }
    }}}
  }}}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weights_blocked(
    Type *tweights, Type *weights, int oc4)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, oc3, ic3, wA, hA, O2, I2, V, V
  MD(Type, aweights, [oc4][this->oc3][this->O2][this->ic4][this->ic3][this->I2][K][K][V][V], weights);
  MD(Type, atweights, [oc4][this->ic4][this->oc3][this->ic3][A][A][this->O2][this->I2][V][V], tweights);

#pragma omp for nowait collapse(6) schedule(static)
  for_each (_oc4, oc4) {
  for_each (_ic4, this->ic4) {
  for_each (_oc3, this->oc3) {
  for_each (_ic3, this->ic3) {
  for_each (_O2, this->O2) {
  for_each (_I2, this->I2) {
    alignas(64) Type aout[A][A][V][V];
    Type *in = (Type *)aweights[_oc4][_oc3][_O2][_ic4][_ic3][_I2];
    using Array = Type[K][K][V][V];
    ker_trans_weights_(aout, *(Array *)in);
    for_each (_wA, A) {
    for_each (_hA, A) {
    for_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        auto *ptarget
          = atweights[_oc4][_ic4][_oc3][_ic3][_wA][_hA][_O2][_I2][_iV];
        if (stream_wei_)
          _mm512_stream_ps(
              ptarget,
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        else
          _mm512_store_ps(
              ptarget,
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
      } else {

#pragma omp simd
        for_each (_oV, V)
          atweights[_oc4][_ic4][_oc3][_ic3][_wA][_hA][_O2][_I2][_iV][_oV]
              = aout[_wA][_hA][_iV][_oV];
      }
    }}}
  }}}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_weights(
    Type *tweights, Type *weights, int oc4)
{
  if (weights_is_bfmt_ || weights_as_bfmt_)
    __trans_weights_blocked(tweights, weights, oc4);
  else
    __trans_weights_plain(tweights, weights, oc4);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weightsa_blocked(
    Type *tweights, Type *weights)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, wA, hA, oc3, ic3, O2, I2, V, V
  MD(Type, aweights, [this->oc4][this->oc3][this->O2][this->ic4][this->ic3][this->I2][K][K][V][V], weights);
  MD(Type, atweights, [this->oc4][this->ic4][A][A][this->oc3][this->ic3][this->O2][this->I2][V][V], tweights);

#pragma omp for nowait collapse(6) schedule(static)
  for_each (_oc4, this->oc4) {
  for_each (_ic4, this->ic4) {
  for_each (_oc3, this->oc3) {
  for_each (_ic3, this->ic3) {
  for_each (_O2, this->O2) {
  for_each (_I2, this->I2) {
    alignas(64) Type aout[A][A][V][V];
    Type *in = (Type *)aweights[_oc4][_oc3][_O2][_ic4][_ic3][_I2];
    using Array = Type[K][K][V][V];
    ker_trans_weights_(aout, *(Array *)in);
    for_each (_wA, A) {
    for_each (_hA, A) {
    for_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        auto *ptarget = atweights[_oc4][_ic4][_wA][_hA][_oc3][_ic3][_O2][_I2][_iV];
        if (stream_wei_)
          _mm512_stream_ps(
              ptarget,
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        else
          _mm512_store_ps(
              ptarget,
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
      } else {
#pragma omp simd
        for_each (_oV, V)
          atweights[_oc4][_ic4][_wA][_hA][_oc3][_ic3][_O2][_I2][_iV][_oV]
              = aout[_wA][_hA][_iV][_oV];
      }
    }}}}
  }}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_weightsa_plain(
    Type * __restrict tweights, Type * __restrict weights)
{
  // oc2, ic2, hK, wK, V, V => oc4, ic4, wA, hA, oc3, ic3, O2, I2, V, V
  MD(Type, aweights, [this->oc4][this->oc3][this->O2][V][this->ic4][this->ic3][this->I2][V][K][K], weights);
  MD(Type, atweights, [this->oc4][this->ic4][A][A][this->oc3][this->ic3][this->O2][this->I2][V][V], tweights);

  int s = this->ic * this->kh * this->kw;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto readin = [&](Type ain[K][K][V][V], Type *wei) {
    MD(Type, awei, [V][this->ic2][V][K][K], wei);

    for_each (_hK, K) {
    for_each (_wK, K) {
    for_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        constexpr auto scale = sizeof(Type);
        auto t = _mm512_i32gather_ps(vindex,
            (float *)&awei[0][0][_iV][_hK][_wK], scale);
        _mm512_store_ps(ain[_hK][_wK][_iV], t);
      } else {
        for_each (_oV, V)
          ain[_hK][_wK][_iV][_oV] = awei[_oV][0][_iV][_hK][_wK];
      }
    }}}
  };

#pragma omp for nowait collapse(6) schedule(static)
  for_each (_oc4, this->oc4) {
  for_each (_ic4, this->ic4) {
  for_each (_oc3, this->oc3) {
  for_each (_ic3, this->ic3) {
  for_each (_O2, this->O2) {
  for_each (_I2, this->I2) {
    alignas(64) Type ain[K][K][V][V];
    alignas(64) Type aout[A][A][V][V];
    readin(ain, (Type *)aweights[_oc4][_oc3][_O2][0][_ic4][_ic3][_I2]);

    ker_trans_weights_(aout, ain);

    for_each (_wA, A) {
    for_each (_hA, A) {
    for_each (_iV, V) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_wei_)
          _mm512_stream_ps(
              atweights[_oc4][_ic4][_wA][_hA][_oc3][_ic3][_O2][_I2][_iV],
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
        else
          _mm512_store_ps(
              atweights[_oc4][_ic4][_wA][_hA][_oc3][_ic3][_O2][_I2][_iV],
              *((__m512 *)&aout[_wA][_hA][_iV][0]));
      } else {
#pragma omp simd
        for_each (_oV, V)
          atweights[_oc4][_ic4][_wA][_hA][_oc3][_ic3][_O2][_I2][_iV][_oV]
              = aout[_wA][_hA][_iV][_oV];
      }
    }}}}
  }}}}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_weightsa(
    Type *tweights, Type *weights)
{
  if (weights_is_bfmt_ || weights_as_bfmt_)
    __trans_weightsa_blocked(tweights, weights);
  else
    __trans_weightsa_plain(tweights, weights);
}


template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_plain(
    Type * __restrict tinput, Type * __restrict input, int _t2, int Tz)
{
  // n, ic, ih, iw => t2 | wA, hA, ic3, I2, T, V
  MD(Type, atinput,[A][A][this->ic3][this->I2][Tz][V], tinput);

  alignas(64) Type aout[A][A][V];
  alignas(64) Type ain[A][A][V];
  int s = this->ih * this->iw;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto readin = [&](int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD(Type, ainput, [this->n][this->ic4][this->ic3][this->I2][V][this->ih][this->iw], input);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    for_each (_hA, A) {
    for_each (_wA, A) {
      if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
          || _wA > _wA_end) {
#pragma omp simd
        for_each (_V, V)
          ain[_hA][_wA][_V] = 0.0f;
      } else {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr int scale = sizeof(Type);
          __m512 t = _mm512_i32gather_ps(vindex,
              (float *)&ainput[_n][0][_ic3][_I2][0][_ih + _hA][_iw + _wA],
              scale);
          _mm512_store_ps(ain[_hA][_wA], t);
        } else {
#pragma omp simd
          for_each (_V, V)
            ain[_hA][_wA][_V]
                = ainput[_n][0][_ic3][_I2][_V][_ih + _hA][_iw + _wA];
        }
      }
    }}
  };

  for_each (_ic3, this->ic3) {
  for_each (_I2, this->I2) {
  for_each (_T, Tz) {
    readin(_ic3, _I2, _T, ain);
    ker_trans_input_(*this, aout, (Type *)ain, 0, 0, 0, -1);

    for_each (_wA, A) {
    for_each (_hA, A) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_in_)
          _mm512_stream_ps(atinput[_wA][_hA][_ic3][_I2][_T],
              *((__m512 *)&aout[_wA][_hA][0]));
        else
          _mm512_store_ps(atinput[_wA][_hA][_ic3][_I2][_T],
              *((__m512 *)&aout[_wA][_hA][0]));
      } else {
#pragma omp simd
        for_each (_V, V)
          atinput[_wA][_hA][_ic3][_I2][_T][_V] = aout[_wA][_hA][_V];
      }
    }}
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_blocked(
    Type * __restrict tinput, Type * __restrict input, int _t2, int Tz)
{
  // n, ic2, ih, iw, V => t2 | wA, hA, ic3, I2, T, V
  MD(Type, ainput, [this->n][this->ic4][this->ic3][this->I2][this->ih][this->iw][V], input);
  MD(Type, atinput, [A][A][this->ic3][this->I2][Tz][V], tinput);

  alignas(64) Type aout[A][A][V];

  for_each (_ic3, this->ic3) {
  for_each (_I2, this->I2) {
  for_each (_T, Tz) {
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    Type *in = ainput[_n][0][_ic3][_I2][_ih][_iw];
    if (_hA_start == 0 && _wA_start == 0 && _hA_end == A - 1
        && _wA_end == A - 1)
      ker_trans_input_(*this, aout, in, 0, A - 1, 0, A - 1);
    else
      ker_trans_input0_(
          *this, aout, in, _hA_start, _hA_end, _wA_start, _wA_end);

    for_each (_wA, A) {
    for_each (_hA, A) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        auto *ptarget = atinput[_wA][_hA][_ic3][_I2][_T];
        if (stream_in_)
          _mm512_stream_ps(ptarget,
              *((__m512 *)&aout[_wA][_hA][0]));
        else
          _mm512_store_ps(ptarget,
              *((__m512 *)&aout[_wA][_hA][0]));
      } else {
#pragma omp simd
        for_each (_V, V)
          atinput[_wA][_hA][_ic3][_I2][_T][_V] = aout[_wA][_hA][_V];
      }
    }}
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_input(
    Type * __restrict tinput, Type * __restrict input, int _t2, int Tz)
{
  if (input_is_bfmt_ || input_as_bfmt_)
    __trans_input_blocked(tinput, input, _t2, Tz);
  else
    __trans_input_plain(tinput, input, _t2, Tz);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_blocked(
    Type * __restrict tinput, Type * __restrict input)
{
  // n, ic2, ih, iw, V => t2, wA, hA, ic3, I2, T, V
  MD(Type, ainput, [this->n][this->ic4][this->ic3][this->I2][this->ih][this->iw][V], input);
  MD(Type, atinput2, [this->t2][A * A * this->T * this->ic], tinput);

#pragma omp for nowait collapse(3)
  for_each (_t2, this->t2) {
  for_each (_ic3, this->ic3) {
  for_each (_I2, this->I2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD(Type, atinput6, [A][A][this->ic3][this->I2][Tz][V], atinput2[_t2]);
    alignas(64) Type aout[A][A][V];

    for_each (_T, Tz) {
      int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
      t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

      Type *in = ainput[_n][0][_ic3][_I2][_ih][_iw];
      if (_hA_start == 0 && _wA_start == 0 && _hA_end == A - 1
          && _wA_end == A - 1)
        ker_trans_input_(*this, aout, in, 0, A - 1, 0, A - 1);
      else
        ker_trans_input0_(
            *this, aout, in, _hA_start, _hA_end, _wA_start, _wA_end);

      for_each (_wA, A) {
        for_each (_hA, A) {
          if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
            if (stream_in_)
              _mm512_stream_ps(atinput6[_wA][_hA][_ic3][_I2][_T],
                               *((__m512 *)&aout[_wA][_hA][0]));
            else
              _mm512_store_ps(atinput6[_wA][_hA][_ic3][_I2][_T],
                              *((__m512 *)&aout[_wA][_hA][0]));
          } else {
#pragma omp simd
            for_each (_V, V)
              atinput6[_wA][_hA][_ic3][_I2][_T][_V] = aout[_wA][_hA][_V];
          }
        }}
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_input_plain(
    Type * __restrict tinput, Type * __restrict input)
{
  // n, ic2, ih, iw, V => t2, wA, hA, ic3, I2, T, V
  MD(Type, atinput2, [this->t2][A * A * this->T * this->ic], tinput);

  int s = this->ih * this->iw;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto readin = [&](int _t2, int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD(Type, ainput, [this->n][this->ic4][this->ic3][this->I2][V][this->ih][this->iw], input);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    for_each (_hA, A) {
    for_each (_wA, A) {
      if (_hA < _hA_start || _hA > _hA_end || _wA < _wA_start
          || _wA > _wA_end) {
#pragma omp simd
        for_each (_V, V)
          ain[_hA][_wA][_V] = 0.0f;
      } else {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr int scale = sizeof(Type);
          __m512 t = _mm512_i32gather_ps(vindex,
              (float *)&ainput[_n][0][_ic3][_I2][0][_ih + _hA][_iw + _wA],
              scale);
          _mm512_store_ps(ain[_hA][_wA], t);
        } else {
#pragma omp simd
          for_each (_V, V)
            ain[_hA][_wA][_V]
                = ainput[_n][0][_ic3][_I2][_V][_ih + _hA][_iw + _wA];
        }
      }
    }}
  };

#pragma omp for nowait collapse(3)
  for_each (_t2, this->t2) {
  for_each (_ic3, this->ic3) {
  for_each (_I2, this->I2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD(Type, atinput6, [A][A][this->ic3][this->I2][Tz][V], atinput2[_t2]);
    alignas(64) Type aout[A][A][V];
    alignas(64) Type ain[A][A][V];

    for_each (_T, Tz) {
      readin(_t2, _ic3, _I2, _T, ain);
      ker_trans_input_(*this, aout, (Type *)ain, 0, 0, 0, -1);

      for_each (_wA, A) {
      for_each (_hA, A) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          if (stream_in_)
            _mm512_stream_ps(atinput6[_wA][_hA][_ic3][_I2][_T],
                *((__m512 *)&aout[_wA][_hA][0]));
          else
            _mm512_store_ps(atinput6[_wA][_hA][_ic3][_I2][_T],
                *((__m512 *)&aout[_wA][_hA][0]));
        } else {
#pragma omp simd
          for_each (_V, V)
            atinput6[_wA][_hA][_ic3][_I2][_T][_V] = aout[_wA][_hA][_V];
        }
      }}
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_input(
    Type *tinput, Type *input)
{
  if (input_is_bfmt_ || input_as_bfmt_)
    __trans_input_blocked(tinput, input);
  else
    __trans_input_plain(tinput, input);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_inputa_blocked(
    Type *tinput, Type *input, int _t2, int _wA, int Tz)
{
  // n, ic2, ih, iw, V => t2, wA | hA, ic3, I2, T, V
  MD(Type, ainput, [this->n][this->ic4][this->ic3][this->I2][this->ih][this->iw][V], input);
  MD(Type, atinput, [A][this->ic3][this->I2][Tz][V], tinput);

  alignas(64) Type aout[A][A][V];

  for_each (_ic3, this->ic3) {
  for_each (_I2, this->I2) {
  for_each (_T, Tz) {
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    Type *in = ainput[_n][0][_ic3][_I2][_ih][_iw];
    if (_hA_start == 0 && _wA_start == 0 && _hA_end == A - 1
        && _wA_end == A - 1) {
      ker_trans_inputa_(*this, aout, in, _wA, 0, A - 1, 0, A - 1);
    } else {
      ker_trans_inputa0_(
          *this, aout, in, _wA, _hA_start, _hA_end, _wA_start, _wA_end);
    }

    for_each (_hA, A) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        auto *ptarget = atinput[_hA][_ic3][_I2][_T];
        if (stream_in_)
          _mm512_stream_ps(
              ptarget, *((__m512 *)&aout[_hA][_wA][0]));
        else
          _mm512_store_ps(
              ptarget, *((__m512 *)&aout[_hA][_wA][0]));
      } else {
#pragma omp simd
        for_each (_V, V)
          atinput[_hA][_ic3][_I2][_T][_V] = aout[_hA][_wA][_V];
      }
    }
  }}}
}
 
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_inputa_plain(
    Type * __restrict tinput, Type * __restrict input, int _t2, int _wA, int Tz)
{
  // n, ic2, ih, iw, V => t2, wA | hA, ic3, I2, T, V
  MD(Type, atinput, [A][this->ic3][this->I2][Tz][V], tinput);

  alignas(64) Type aout[A][A][V];
  alignas(64) Type ain[A][A][V];
  int s = this->ih * this->iw;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto readin = [&](int _ic3, int _I2, int _T, Type ain[A][A][V]) {
    MD(Type, ainput, [this->n][this->ic4][this->ic3][this->I2][V][this->ih][this->iw], input);
    int _n, _ih, _iw, _hA_start, _wA_start, _hA_end, _wA_end;
    t2spati(_t2, _T, _n, _ih, _iw, _hA_start, _hA_end, _wA_start, _wA_end);

    for_each (__wA, A) {
    for_each (__hA, A) {
      if (__hA < _hA_start || __hA > _hA_end || __wA < _wA_start
          || __wA > _wA_end) {
#pragma omp simd
        for_each (_V, V)
          ain[__hA][__wA][_V] = 0.0f;
      } else {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          constexpr int scale = sizeof(Type);
          __m512 t = _mm512_i32gather_ps(vindex,
              (float *)&ainput[_n][0][_ic3][_I2][0][_ih + __hA][_iw + __wA],
              scale);
          _mm512_store_ps(ain[__hA][__wA], t);
        } else {
#pragma omp simd
          for_each (_V, V)
            ain[__hA][__wA][_V]
                = ainput[_n][0][_ic3][_I2][_V][_ih + __hA][_iw + __wA];
        }
      }
    }}
  };

  for_each (_ic3, this->ic3) {
  for_each (_I2, this->I2) {
  for_each (_T, Tz) {
    readin(_ic3, _I2, _T, ain);
    ker_trans_inputa_(*this, aout, (Type *)ain, _wA, 0, A - 1, 0, -1);

    for_each (_hA, A) {
      if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
        if (stream_in_)
          _mm512_stream_ps(
              atinput[_hA][_ic3][_I2][_T], *((__m512 *)&aout[_hA][_wA][0]));
        else
          _mm512_store_ps(
              atinput[_hA][_ic3][_I2][_T], *((__m512 *)&aout[_hA][_wA][0]));
      } else {
#pragma omp simd
        for_each (_V, V)
          atinput[_hA][_ic3][_I2][_T][_V] = aout[_hA][_wA][_V];
      }
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_inputa(
    Type *tinput, Type *input, int _t2, int _wA, int Tz)
{
  if(input_is_bfmt_ || input_as_bfmt_)
    __trans_inputa_blocked(tinput, input, _t2, _wA, Tz);
  else
    __trans_inputa_plain(tinput, input, _t2, _wA, Tz);
}

// tweights:     oc4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:       t2 | A, A, ic3, I2, T, V
// toutput:  t2, oc4 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::gemm(
    Type *toutput, Type *tinput, Type *tweights, int _t2, int Tz)
{
  auto ker_gemm = (_t2 == this->t2 - 1) ? ker_gemm0_ : ker_gemm_;

  MD(Type, atinput, [A][A][this->ic3][this->I2][Tz][V], tinput);
  MD(Type, atoutput, [A][A][this->oc3][this->O2][Tz][V], toutput);
  MD(Type, atweights, [this->oc3][this->ic3][A][A][this->O2][this->I2][V][V],
      tweights);

  for_each (_wA, A) {
    for_each (_hA, A) {
      for_each (_oc3, this->oc3) {
        for_each (_ic3, this->ic3) {
          ker_gemm(*this, (Type *)atoutput[_wA][_hA][_oc3],
              (Type *)atinput[_wA][_hA][_ic3],
              (Type *)atweights[_oc3][_ic3][_wA][_hA], _ic3 == 0);
        }
      }
    }
  }
}

// tweights: oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2, A, A, ic3, I2, T, V
// toutput:  t2, A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::gemm(
    Type * __restrict toutput, Type * __restrict tinput, Type * __restrict tweights)
{
  MD(Type, atinput2, [this->t2][A * A * this->T * this->ic], tinput);
  MD(Type, atoutput2, [this->t2][A * A * this->T * this->oc3 * this->O2 * V], toutput);
  MD(Type, atweights, [this->oc3][this->ic3][A][A][this->O2][this->I2][V][V],
      tweights);

#pragma omp for nowait collapse(4)
  for_each (_t2, this->t2) {
    for_each (_wA, A) {
      for_each (_hA, A) {
        for_each (_oc3, this->oc3) {
          for_each (_ic3, this->ic3) {
            int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
            auto ker_gemm = (_t2 == this->t2 - 1) ? ker_gemm0_ : ker_gemm_;
            MD(Type, atinput6, [A][A][this->ic3][this->I2][Tz][V],
                atinput2[_t2]);
            MD(Type, atoutput6, [A][A][this->oc3][this->O2][Tz][V],
                atoutput2[_t2]);
            ker_gemm(*this, (Type *)atoutput6[_wA][_hA][_oc3],
                (Type *)atinput6[_wA][_hA][_ic3],
                (Type *)atweights[_oc3][_ic3][_wA][_hA], _ic3 == 0);
          }
        }
      }
    }
  }
}

// tweights:    oc4, A | A, oc3, ic3, O2, I2, V, V
// tinputs:      t2, A | A, ic3, I2, T, V
// toutput: t2, oc4, A | A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::gemma(
    Type * __restrict toutput, Type * __restrict tinput, Type *tweights, int _t2, int Tz)
{
  auto ker_gemm = (_t2 == this->t2 - 1) ? ker_gemm0_ : ker_gemm_;

  MD(Type, atinput,  [A][this->ic3][this->I2][Tz][V], tinput);
  MD(Type, atoutput, [A][this->oc3][this->O2][Tz][V], toutput);
  MD(Type, atweights, [A][this->oc3][this->ic3][this->O2][this->I2][V][V], tweights);

  for_each (_hA, A) {
  for_each (_oc3, this->oc3) {
  for_each (_ic3, this->ic3) {
      ker_gemm(*this, (Type *)atoutput[_hA][_oc3], (Type *)atinput[_hA][_ic3],
          (Type *)atweights[_hA][_oc3][_ic3], _ic3 == 0);
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_plain(
    Type * __restrict output, Type * __restrict toutput, Type * __restrict bias
    , int _t2, int Tz)
{
  // A, A, oc3, O2, T, V -> n, oc, oh, ow
  MD(Type, atoutput, [A][A][this->oc3][this->O2][Tz][V], toutput);
  MD(Type, abias, [this->oc3][this->O2][V], bias);

  alignas(64) Type ain[A][A][V];
  alignas(64) Type aout[A - K + 1][A - K + 1][V];

  int s = this->oh * this->ow;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto writeout = [&](int _oc3, int _O2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD(Type, aoutput, [this->n][this->oc4][this->oc3][this->O2][V][this->oh][this->ow], output);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m512 t = _mm512_load_ps(aout[_hA][_wA]);
          constexpr int scale = sizeof(Type);
          _mm512_i32scatter_ps(
              (float *)&aoutput[_n][0][_oc3][_O2][0][_oh + _hA][_ow + _wA],
              vindex, t, scale);
        } else {
#pragma omp simd
          for_each (_V, V)
            aoutput[_n][0][_oc3][_O2][_V][_oh + _hA][_ow + _wA]
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

  for_each (_oc3, this->oc3) {
  for_each (_O2, this->O2) {
  for_each (_T, Tz) {
    for_each (_wA, A) {
    for_each (_hA, A) {
#pragma omp simd
    for_each (_V, V) {
      ain[_wA][_hA][_V] = atoutput[_wA][_hA][_oc3][_O2][_T][_V];
    }}}

    ker_trans_output_(*this, (Type *)aout, ain, abias[_oc3][_O2], 0, -1);

    writeout(_oc3, _O2, _T, aout);
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_blocked(
    Type *output, Type *toutput, Type *bias, int _t2, int Tz)
{
  // A, A, oc3, O2, T, V -> n, oc2, oh, ow, V
  MD(Type, atoutput, [A][A][this->oc3][this->O2][Tz][V], toutput);
  MD(Type, aoutput, [this->n][this->oc4][this->oc3][this->O2][this->oh][this->ow][V], output);
  MD(Type, abias, [this->oc3][this->O2][V], bias);

  alignas(64) Type ain[A][A][V];

  for_each (_oc3, this->oc3) {
  for_each (_O2, this->O2) {
  for_each (_T, Tz) {
    for_each (_wA, A) {
    for_each (_hA, A) {
#pragma omp simd
    for_each (_V, V) {
      ain[_wA][_hA][_V] = atoutput[_wA][_hA][_oc3][_O2][_T][_V];
    }}}

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
    Type *out = aoutput[_n][0][_oc3][_O2][_oh][_ow];

    if (_hOA_end < A - K || _wOA_end < A - K)
      ker_trans_output0_(*this, out, ain, abias[_oc3][_O2], _hOA_end, _wOA_end);
    else
      ker_trans_output_(*this, out, ain, abias[_oc3][_O2], A - K, A - K);
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_output(
    Type *output, Type *toutput, Type *bias, int _t2, int Tz)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_output_blocked(output, toutput, bias, _t2, Tz);
  else
    __trans_output_plain(output, toutput, bias, _t2, Tz);
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_blocked(Type *output,
    Type *output_tmp, Type *toutput, Type *bias, int _t2, int Tz, int ic4,
    int oc4, bool inline_reduce)
{
  MD(Type, atoutput, [A][A][this->ic4][this->oc3 * this->O2 / this->ic4][Tz][V], toutput);
  MD(Type, aoutput, [this->n][this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][this->oh][this->ow][V], output);
  MD(Type, aoutput_tmp, [this->n][this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][this->oh][this->ow][V], output_tmp);
  MD(Type, aroutput, [this->ic4][this->n][this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][this->oh][this->ow][V], routput_);
  MD(Type, abias, [this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][V], bias);

  // TODO: pause
  auto sync_on = [this](int _t2, int oc4) {
    MD(unsigned char, cntr, [this->t2][this->oc4], routput_cntr_);
#pragma omp atomic
    cntr[_t2][oc4]++;
    unsigned char c = 0;
#pragma omp atomic read
    c = cntr[_t2][oc4];
    while (c != this->ic4) {
      _mm_pause();
#pragma omp atomic read
      c = cntr[_t2][oc4];
    }
  };

  alignas(64) Type ain[A][A][V];

  for_each (_ic4, this->ic4) {
    for_each (_oc, this->oc3 * this->O2/this->ic4) {
      for_each (_T, Tz) {
        for_each (_wA, A) {
        for_each (_hA, A) {
#pragma omp simd
        for_each (_V, V) {
          ain[_wA][_hA][_V] = atoutput[_wA][_hA][_ic4][_oc][_T][_V];
        }}}
        int _n, _oh, _ow, _hOA_end, _wOA_end;
        t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
        Type *out = aoutput_tmp[_n][oc4][_ic4][_oc][_oh][_ow];

        if (bias == nullptr) {
          if (_hOA_end < A - K || _wOA_end < A - K)
            ker_trans_output0_nobias_(*this, out, ain, nullptr, _hOA_end, _wOA_end);
          else
            ker_trans_output_nobias_(*this, out, ain, nullptr, A - K, A - K);
        } else {
          if (_hOA_end < A - K || _wOA_end < A - K)
            ker_trans_output0_(*this, out, ain, (Type *)abias[oc4][_ic4][_oc],
                _hOA_end, _wOA_end);
          else
            ker_trans_output_(
                *this, out, ain, (Type *)abias[oc4][_ic4][_oc], A - K, A - K);
        }
      }
    }
  }

  if (inline_reduce) {
    sync_on(_t2, oc4);

    for_each (_oc, this->oc3 * this->O2 / this->ic4) {
    for_each (_T, Tz) {
      int _n, _oh, _ow, _hOA_end, _wOA_end;
      t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
      for_each (_hA, _hOA_end + 1) {
      for_each (_wA, _wOA_end + 1) {
      for (int __ic4 = 1; __ic4 < this->ic4; __ic4++) {
#pragma omp simd
      for_each (_V, V) {
        aoutput[_n][oc4][ic4][_oc][_oh + _hA][_ow + _wA][_V] +=
          aroutput[__ic4 - 1][_n][oc4][ic4][_oc][_oh + _hA][_ow + _wA][_V];
      }}}}
    }}
  }
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_plain(Type *output,
    Type * __restrict output_tmp, Type * __restrict toutput, Type *bias, int _t2, int Tz, int ic4,
    int oc4, bool inline_reduce)
{
  MD(Type, atoutput, [A][A][this->ic4][this->oc3 * this->O2 / this->ic4][Tz][V], toutput);
  MD(Type, aroutput, [this->ic4][this->n][this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][this->oh][this->ow][V], routput_);
  MD(Type, abias, [this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][V], bias);

  alignas(64) Type ain[A][A][V];
  alignas(64) Type aout[A - K + 1][A - K + 1][V];

  int s = this->oh * this->ow;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto writeout = [&](int _ic4, int _oc, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD(Type, aoutput, [this->n][this->oc4][this->ic4][this->oc3 * this->O2 / this->ic4][V][this->oh][this->ow], output_tmp);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m512 t = _mm512_load_ps(aout[_hA][_wA]);
          constexpr auto scale = sizeof(Type);
          _mm512_i32scatter_ps(
              (float *)&aoutput[_n][oc4][_ic4][_oc][0][_oh + _hA][_ow + _wA],
              vindex, t, scale);
        } else {
#pragma omp simd
          for_each (_V, V)
            aoutput[_n][oc4][_ic4][_oc][_V][_oh + _hA][_ow + _wA]
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

  for_each (_ic4, this->ic4) {
  for_each (_oc, this->oc3 * this->O2/this->ic4) {
  for_each (_T, Tz) {
    for_each (_wA, A) {
    for_each (_hA, A) {
#pragma omp simd
    for_each (_V, V) {
      ain[_wA][_hA][_V] = atoutput[_wA][_hA][_ic4][_oc][_T][_V];
    }}}

    if (bias == nullptr)
      ker_trans_output_nobias_(*this, (Type *)aout, ain, nullptr, 0, -1);
    else
      ker_trans_output_(
          *this, (Type *)aout, ain, (Type *)abias[oc4][_ic4][_oc], 0, -1);

    writeout(_ic4, _oc, _T, aout);
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_output(Type *output,
    Type *output_tmp, Type *toutput, Type *bias, int _t2, int Tz, int ic4,
    int oc4, bool inline_reduce)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_output_blocked(
        output, output_tmp, toutput, bias, _t2, Tz, ic4, oc4, inline_reduce);
  else
    __trans_output_plain(
        output, output_tmp, toutput, bias, _t2, Tz, ic4, oc4, false);
}

// toutput:  mthr | hA/A, oc3, O2, T, V
// toutputa: t2, oc4 | oc3, O2, T, wA/A | hA/A-K+1, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_outputa_th(
    Type *toutputa, Type *toutput, int Tz)
{
  MD(Type, atoutput, [A][this->oc3 * this->O2][Tz][V], toutput);
  MD(Type, atoutputa, [this->oc3 * this->O2][Tz][A][(A - K + 1) * V], toutputa);

  for_each (_oc, this->oc3 * this->O2) {
    for_each (_T, Tz) {
      ker_trans_outputa_th_(
          *this, atoutputa[_oc][_T][0], atoutput[0][_oc][_T], Tz, stream_out_);
    }
  }
}

// output: n, oc2, h, w, V
// toutputa: t2, oc2, T, wA/A | hA/A-K+1, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_outputa_bh_blocked(
    Type *output, Type *toutputa, Type *bias)
{
  MD(Type, aoutput, [this->n][this->oc2][this->oh][this->ow][V], output);
  MD(Type, abias, [this->oc2][V], bias);
  MD(Type, atoutputa2, [this->t2][A * (A - K + 1) * this->T * this->oc], toutputa);

#pragma omp for nowait collapse(2)
  for_each (_t2, this->t2) {
  for_each (_oc2, this->oc2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD(Type, atoutputa3, [this->oc2][Tz][A * (A - K + 1) * V], atoutputa2[_t2]);

    for_each (_T, Tz) {
      int _n, _oh, _ow, _hOA_end, _wOA_end;
      t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
      Type *out = aoutput[_n][_oc2][_oh][_ow];
      using Array1 = Type[A][A - K + 1][V];
      Array1 *in = (Array1 *)atoutputa3[_oc2][_T];

      if (_hOA_end < A - K || _wOA_end < A - K)
        ker_trans_outputa0_bh_(*this, out, *in, abias[_oc2], _hOA_end, _wOA_end);
      else
        ker_trans_outputa_bh_(*this, out, *in, abias[_oc2], A - K, A - K);
    }
  }}
}

// output: n, oc, h, w
// toutputa: t2, oc2, T, wA/A | hA/A-K+1, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_outputa_bh_plain(
    Type * __restrict output, Type * __restrict toutputa, Type *bias)
{
  MD(Type, abias, [this->oc2][V], bias);
  MD(Type, atoutputa2, [this->t2][A * (A - K + 1) * this->T * this->oc], toutputa);

  int s = this->oh * this->ow;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto writeout = [&](int _t2, int _oc2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD(Type, aoutput, [this->n][this->oc2][V][this->oh][this->ow], output);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m512 t = _mm512_load_ps(aout[_hA][_wA]);
          constexpr auto scale = sizeof(Type);
          _mm512_i32scatter_ps(
              (float *)&aoutput[_n][_oc2][0][_oh + _hA][_ow + _wA],
              vindex, t, scale);
        } else {
#pragma omp simd
          for_each (_V, V)
            aoutput[_n][_oc2][_V][_oh + _hA][_ow + _wA]
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

#pragma omp for nowait collapse(2)
  for_each (_t2, this->t2) {
  for_each (_oc2, this->oc2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD(Type, atoutputa3, [this->oc2][Tz][A * (A - K + 1) * V], atoutputa2[_t2]);
    alignas(64) Type aout[A - K + 1][A - K + 1][V];

    for_each (_T, Tz) {
      using Array1 = Type[A][A - K + 1][V];
      Array1 *in = (Array1 *)atoutputa3[_oc2][_T];

      ker_trans_outputa_bh_(*this, (Type *)aout, *in, abias[_oc2], 0, -1);

      writeout(_t2, _oc2, _T, aout);
    }
  }}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_outputa_bh(
    Type *output, Type *toutputa, Type *bias)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_outputa_bh_blocked(output, toutputa, bias);
  else
    __trans_outputa_bh_plain(output, toutputa, bias);

}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_blocked(
    Type *output, Type *toutput, Type *bias)
{
  // A, A, oc3, O2, T, V -> n, oc2, oh, ow, V
  MD(Type, aoutput, [this->n][this->oc4][this->oc3][this->O2][this->oh][this->ow][V], output);
  MD(Type, atoutput2, [this->t2][A * A * this->T * this->oc3 * this->O2 * V], toutput);
  MD(Type, abias, [this->oc3][this->O2][V], bias);

#pragma omp for nowait collapse(3)
  for_each (_t2, this->t2) {
  for_each (_oc3, this->oc3) {
  for_each (_O2, this->O2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD(Type, atoutput, [A][A][this->oc3][this->O2][Tz][V], atoutput2[_t2]);
    alignas(64) Type ain[A][A][V];

    for_each (_T, Tz) {
      for_each (_wA, A) {
      for_each (_hA, A) {
#pragma omp simd
      for_each (_V, V) {
        ain[_wA][_hA][_V] = atoutput[_wA][_hA][_oc3][_O2][_T][_V];
      }}}

      int _n, _oh, _ow, _hOA_end, _wOA_end;
      t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);
      Type *out = aoutput[_n][0][_oc3][_O2][_oh][_ow];

      if (_hOA_end < A - K || _wOA_end < A - K)
        ker_trans_output0_(*this, out, ain, abias[_oc3][_O2], _hOA_end, _wOA_end);
      else
        ker_trans_output_(*this, out, ain, abias[_oc3][_O2], A - K, A - K);
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__trans_output_plain(
    Type * __restrict output, Type * __restrict toutput, Type *bias)
{
  // A, A, oc3, O2, T, V -> n, oc, oh, ow
  MD(Type, abias, [this->oc3][this->O2][V], bias);
  MD(Type, atoutput2, [this->t2][A * A * this->T * this->oc3 * this->O2 * V], toutput);

  int s = this->oh * this->ow;
  const __m512i vindex
      = _mm512_set_epi32(15 * s, 14 * s, 13 * s, 12 * s, 11 * s, 10 * s,
          9 * s, 8 * s, 7 * s, 6 * s, 5 * s, 4 * s, 3 * s, 2 * s, s, 0);

  auto writeout = [&](int _t2, int _oc3, int _O2, int _T,
                      Type aout[A - K + 1][A - K + 1][V]) {
    MD(Type, aoutput, [this->n][this->oc4][this->oc3][this->O2][V][this->oh][this->ow], output);

    int _n, _oh, _ow, _hOA_end, _wOA_end;
    t2spato(_t2, _T, _n, _oh, _ow, _hOA_end, _wOA_end);

    for (int _wA = 0; _wA <= _wOA_end; ++_wA) {
      for (int _hA = 0; _hA <= _hOA_end; ++_hA) {
        if (I == ISA_SKX_AVX512 && std::is_same<Type, float>::value) {
          __m512 t = _mm512_load_ps(aout[_hA][_wA]);
          constexpr auto scale = sizeof(Type);
          _mm512_i32scatter_ps(
              (float *)&aoutput[_n][0][_oc3][_O2][0][_oh + _hA][_ow + _wA],
              vindex, t, scale);
        } else {
#pragma omp simd
          for_each (_V, V)
            aoutput[_n][0][_oc3][_O2][_V][_oh + _hA][_ow + _wA]
                = aout[_hA][_wA][_V];
        }
      }
    }
  };

#pragma omp for nowait collapse(3)
  for_each (_t2, this->t2) {
  for_each (_oc3, this->oc3) {
  for_each (_O2, this->O2) {
    int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
    MD(Type, atoutput6, [A][A][this->oc3][this->O2][Tz][V], atoutput2[_t2]);
    alignas(64) Type ain[A][A][V];
    alignas(64) Type aout[A - K + 1][A - K + 1][V];

    for_each (_T, Tz) {
      for_each (_wA, A) {
      for_each (_hA, A) {
#pragma omp simd
      for_each (_V, V) {
        ain[_wA][_hA][_V] = atoutput6[_wA][_hA][_oc3][_O2][_T][_V];
      }}}

      ker_trans_output_(*this, (Type *)aout, ain, abias[_oc3][_O2], 0, -1);

      writeout(_t2, _oc3, _O2, _T, aout);
    }
  }}}
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::trans_output(
    Type *output, Type *toutput, Type *bias)
{
  if (output_is_bfmt_ || output_as_bfmt_)
    __trans_output_blocked(output, toutput, bias);
  else
    __trans_output_plain(output, toutput, bias);
}

// tweights: oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2 | A, A, ic3, I2, T, V
// toutput:  t2 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a040(
    Type *output, Type *input, Type *weights, Type *bias)
{
  MD(Type, atinput2, [mthr_][A * A * this->T * this->ic], tinput_);
  MD(Type, atoutput2, [mthr_][A * A * this->T * this->oc], toutput_);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(tweights_, weights);
#pragma omp barrier
    }
#pragma omp for nowait collapse(1)
    for_each (_t2, this->t2) {
      int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
      size_t ithr = omp_get_thread_num();

      trans_input(atinput2[ithr], input, _t2, Tz);
      gemm(atoutput2[ithr], atinput2[ithr], tweights_, _t2, Tz);
      trans_output(output, atoutput2[ithr], bias, _t2, Tz);
    }
  }
  if (inference_acc_) is_first_run_ = false;
}

// tweights:     oc4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2      | A, A, ic3, I2, T, V
// toutput:  t2, oc4 | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a061(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, atinput2, [mthr_][A * A * this->T * this->ic], tinput_);
  MD(Type, atoutput2, [mthr_][A * A * this->T * this->oc3 * this->O2 * V], toutput_);
  MD(Type, atweights2, [this->oc4][A * A * this->ic * this->oc3 * this->O2 * V], tweights_);

  MD(Type, aoutput, [this->n][this->oc4][this->oh * this->ow * this->oc3 * this->O2 * V], output);
  MD(Type, abias, [this->oc4][this->oc3 * this->O2 * V], bias);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(tweights_, weights, this->oc4);
#pragma omp barrier
    }
#pragma omp for nowait collapse(2)
    for_each (_t2, this->t2) {
      for_each (_oc4, this->oc4) {
        int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
        size_t ithr = omp_get_thread_num();

        trans_input(atinput2[ithr], input, _t2, Tz);
        gemm(atoutput2[ithr], atinput2[ithr], atweights2[_oc4], _t2, Tz);
        trans_output(aoutput[0][_oc4], atoutput2[ithr], abias[_oc4], _t2, Tz);
      }
    }
  }
  if (inference_acc_) is_first_run_ = false;
}

// tweights:     oc4, wA | hA, oc3, ic3, O2, I2, V, V
// tinputa:  t2,      wA | hA, ic3, I2, T, V
// toutput:  t2, oc4, wA | hA, oc3, O2, T, V
// toutputa: t2, oc4, oc3, O2, T, wA, hA, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a0e1(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, atinputa2, [mthr_][A * this->T * this->ic], tinput_);
  MD(Type, atoutput2, [mthr_][A * this->T * this->oc3 * this->O2 * V], toutput_);
  MD(Type, atoutputa2, [this->t2][this->oc * A * (A - K + 1) * this->T], toutputa_);
  MD(Type, atweights3, [this->oc4][A][A * this->ic * this->oc3 * this->O2 * V], tweights_);
  MD(Type, aoutput, [this->n][this->oc4][this->oh * this->ow * this->oc3 * this->O2 * V], output);
  MD(Type, abias, [this->oc4][this->oc3 * this->O2 * V], bias);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weightsa(tweights_, weights);
#pragma omp barrier
    }
#pragma omp for nowait collapse(3)
    for_each (_t2, this->t2) {
      for_each (_oc4, this->oc4) {
        for_each (_wA, A) {
          int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
          size_t ithr = omp_get_thread_num();

          MD(Type, atoutputa6, [this->oc4][this->oc3][this->O2][Tz][A][(A - K + 1) * V], atoutputa2[_t2]);
          trans_inputa(atinputa2[ithr], input, _t2, _wA, Tz);
          gemma(atoutput2[ithr], atinputa2[ithr], atweights3[_oc4][_wA], _t2, Tz);
          trans_outputa_th(atoutputa6[_oc4][0][0][0][_wA], atoutput2[ithr], Tz);
        }
      }
    }
#pragma omp barrier
    trans_outputa_bh(output, toutputa_, bias);
  }
  if (inference_acc_) is_first_run_ = false;
}

// tweights:     oc4, wA | hA, oc3, ic3, O2, I2, V, V
// tinputa:  t2,      wA | hA, ic3, I2, T, V
// toutput:  t2, oc4, wA | hA, oc3, O2, T, V
// toutputa: t2, oc4, oc3, O2, T, wA, hA, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a0e0(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, atinput2, [this->t2][A * A * this->T * this->ic], tinput_);
  MD(Type, atoutput2, [mthr_][A * this->T * this->oc3 * this->O2 * V], toutput_);
  MD(Type, atoutputa2, [this->t2][this->oc * A * (A - K + 1) * this->T], toutputa_);
  MD(Type, atweights3, [this->oc4][A][A * this->ic * this->oc3 * this->O2 * V], tweights_);
  MD(Type, aoutput, [this->n][this->oc4][this->oh * this->ow * this->oc3 * this->O2 * V], output);
  MD(Type, abias, [this->oc4][this->oc3 * this->O2 * V], bias);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weightsa(tweights_, weights);
    }
    trans_input(tinput_, input);
#pragma omp barrier

#pragma omp for nowait collapse(3)
    for_each (_t2, this->t2) {
      for_each (_oc4, this->oc4) {
        for_each (_wA, A) {
          int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
          size_t ithr = omp_get_thread_num();

          MD(Type, atoutputa6, [this->oc4][this->oc3][this->O2][Tz][A][(A - K + 1) * V], atoutputa2[_t2]);
          MD(Type, atinputa2, [A][A * Tz * this->ic], atinput2[_t2]);
          gemma(atoutput2[ithr], atinputa2[_wA], atweights3[_oc4][_wA], _t2, Tz);
          trans_outputa_th(atoutputa6[_oc4][0][0][0][_wA], atoutput2[ithr], Tz);
        }
      }
    }
#pragma omp barrier
    trans_outputa_bh(output, toutputa_, bias);
  }
  if (inference_acc_) is_first_run_ = false;
}


// tweights:     oc4, ic4 | oc3, ic3, A, A, O2, I2, V, V
// tinputs:  t2,      ic4 | A, A, ic3, I2, T, V
// toutput:  t2, oc4      | A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a073(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, atinput2, [mthr_][A * A * this->T * this->ic3 * this->I2 * V], tinput_);
  MD(Type, atoutput2, [mthr_][A * A * this->T * this->oc3 * this->O2 * V], toutput_);
  MD(Type, atweights2, [this->oc4][this->ic4][A * A * this->ic3 * this->I2 * V * this->oc3 * this->O2 * V], tweights_);

  MD(Type, ainput, [this->n][this->ic4][this->ih * this->iw * this->ic3 * this->I2 * V], input);
  MD(Type, aroutput, [this->ic4][this->n][this->oc4][this->oh * this->ow * this->oc3 * this->O2][V], routput_);
  MD(Type, aoutput, [this->n][this->oc4][this->oh * this->ow * this->oc3 * this->O2][V], output);
  MD(Type, abias, [this->oc4][this->oc3 * this->O2 * V], bias);

  bool inline_reduce = (output_is_bfmt_ || output_as_bfmt_) && (this->ic4 > 1)
      && ((this->O2 * this->oc3) % 2 == 0)
      && (mthr_ >= this->t2 * this->oc4 * this->ic4);

  memset(routput_cntr_, 0, this->t2 * this->oc4);

#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights(tweights_, weights, this->oc4);
#pragma omp barrier
    }
#pragma omp for nowait collapse(3)
    for_each (_t2, this->t2) {
      for_each (_oc4, this->oc4) {
        for_each (_ic4, this->ic4) {
          int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
          size_t ithr = omp_get_thread_num();
          trans_input(atinput2[ithr], ainput[0][_ic4], _t2, Tz);
          gemm(atoutput2[ithr], atinput2[ithr], atweights2[_oc4][_ic4], _t2, Tz);
          if (_ic4 == 0)
            trans_output(output, output, atoutput2[ithr], bias, _t2, Tz, _ic4,
                _oc4, inline_reduce);
          else
            trans_output(output, (Type *)aroutput[_ic4 - 1], atoutput2[ithr],
                nullptr, _t2, Tz, _ic4, _oc4, inline_reduce);
        }
      }
    }
    if (!inline_reduce) {
#pragma omp barrier
#pragma omp for nowait collapse(3)
      for (size_t _n = 0; _n < this->n; ++_n) {
        for (size_t _oc4 = 0; _oc4 < this->oc4; ++_oc4) {
          for (size_t _i = 0; _i < this->oc3 * this->O2 * this->oh * this->ow;
               ++_i) {
            for (size_t _ic4 = 1; _ic4 < this->ic4; ++_ic4) {
#pragma omp simd
              for (size_t _V = 0; _V < V; ++_V) {
                aoutput[_n][_oc4][_i][_V]
                    += aroutput[_ic4 - 1][_n][_oc4][_i][_V];
              }
            }
          }
        }
      }
    }
  }
  if (inference_acc_)
    is_first_run_ = false;
}

// Thread-teaming along 't' dimension.
// Fuse trans-input, gemm and trans-output along 't' dimension
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a448(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, atinput3, [this->nteams][this->nthreads][A * A * this->T * this->ic], tinput_);
  MD(Type, atoutput3, [this->nteams][this->nthreads][A * A * this->T * this->oc], toutput_);
  MD(Type, atweights2, [this->nteams][this->oc * this->ic * A * A], tweights_);

  omp_set_nested(1);
#pragma omp parallel num_threads(this->nteams) proc_bind(spread)
#pragma omp for nowait collapse(1) schedule(static)
  for (int s = 0; s < this->nteams; s++)
#pragma omp parallel num_threads(this->nthreads) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights((Type *)atweights2[s], weights);
#pragma omp barrier
    }
#pragma omp for nowait collapse(1) schedule(static)
    for (int _t2 = ttm_[s].start; _t2 <= ttm_[s].end; _t2++) {
      int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
      size_t ithr = omp_get_thread_num();

      trans_input(atinput3[s][ithr], input, _t2, Tz);
      gemm(atoutput3[s][ithr], atinput3[s][ithr], atweights2[s], _t2, Tz);
      trans_output(output, atoutput3[s][ithr], bias, _t2, Tz);
    }
  }
  if (inference_acc_) is_first_run_ = false;
}

// Flat mode
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a000(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
#pragma omp parallel num_threads(mthr_) proc_bind(close)
  {
    if (is_first_run_)
      trans_weights(tweights_, weights);
    trans_input(tinput_, input);
#pragma omp barrier
    gemm(toutput_, tinput_, tweights_);
#pragma omp barrier
    trans_output(output, toutput_, bias);
  }
  if (inference_acc_) is_first_run_ = false;
}

// Thread teaming along 'o' dimension.
// Flat mode (no-fusion)
//
// tweights: nteams | oc3, ic3, A, A, O2, I2, V, V (oc3 /= nteams)
// tinputs:  nteams | t2, A, A, ic3, I2, T, V (dup)
// toutput:  nteams | t2, A, A, oc3, O2, T, V
template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a201(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, aoutput3, [this->n][this->nteams][this->oc * this->oh * this->ow / this->nteams], output);
  MD(Type, aweights2, [this->nteams][this->oc * this->ic * K * K / this->nteams], weights);
  MD(Type, abias2, [this->nteams][this->oc / this->nteams], bias);
  MD(Type, atinput2, [this->nteams][this->t2 * A * A * this->T * this->ic], tinput_);
  MD(Type, atoutput2, [this->nteams][this->t2 * A * A * this->T * this->oc / this->nteams], toutput_);
  MD(Type, atweights2, [this->nteams][this->oc * this->ic * A * A / this->nteams], tweights_);

  omp_set_nested(1);
#pragma omp parallel num_threads(this->nteams) proc_bind(spread)
#pragma omp for nowait collapse(1) schedule(static)
  for (int s = 0; s < this->nteams; s++)
#pragma omp parallel num_threads(this->nthreads) proc_bind(close)
  {
    if (is_first_run_)
      trans_weights((Type *)atweights2[s], (Type *)aweights2[s]);
    trans_input((Type *)atinput2[s], input);
#pragma omp barrier
    gemm((Type *)atoutput2[s], (Type *)atinput2[s], (Type *)atweights2[s]);
#pragma omp barrier
    trans_output((Type *)aoutput3[0][s], (Type *)atoutput2[s], (Type *)abias2[s]);
  }
  if (inference_acc_) is_first_run_ = false;
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::__execute_a241(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  MD(Type, aoutput3, [this->n][this->nteams][this->oc * this->oh * this->ow / this->nteams], output);
  MD(Type, aweights2, [this->nteams][this->oc * this->ic * K * K / this->nteams], weights);
  MD(Type, abias2, [this->nteams][this->oc / this->nteams], bias);
  MD(Type, atinput3, [this->nteams][this->nthreads][A * A * this->T * this->ic], tinput_);
  MD(Type, atoutput3, [this->nteams][this->nthreads][A * A * this->T * this->oc / this->nteams], toutput_);
  MD(Type, atweights2, [this->nteams][this->oc * this->ic * A * A / this->nteams], tweights_);

  omp_set_nested(1);
#pragma omp parallel num_threads(this->nteams) proc_bind(spread)
#pragma omp for nowait collapse(1) schedule(static)
  for (int s = 0; s < this->nteams; s++)
#pragma omp parallel num_threads(this->nthreads) proc_bind(close)
  {
    if (is_first_run_) {
      trans_weights((Type *)atweights2[s], (Type *)aweights2[s]);
#pragma omp barrier
    }
#pragma omp for nowait collapse(1)
    for_each (_t2, this->t2) {
      int Tz = _t2 == (this->t2 - 1) ? this->Tr : this->T;
      size_t ithr = omp_get_thread_num();

      trans_input(atinput3[s][ithr], input, _t2, Tz);
      gemm(atoutput3[s][ithr], atinput3[s][ithr], atweights2[s], _t2, Tz);
      trans_output(aoutput3[0][s], atoutput3[s][ithr], abias2[s], _t2, Tz);
    }
  }
  if (inference_acc_) is_first_run_ = false;
}

template <typename Type, const int A, const int K, const int V, const int I>
void elx_conv_wino_t<Type, A, K, V, I>::execute(
    Type * __restrict output, Type * __restrict input, Type * __restrict weights, Type * __restrict bias)
{
  if (is_bfmt_)
    return (this->*execute_opt_)(output, input, weights, bias);
  else {
    Type *in = input;
    Type *wei = weights;
    Type *out = output_as_bfmt_ ? boutput_ : output;

    if (input_as_bfmt_) {
      MD(Type, abinput, [this->n][this->ic2][this->ih][this->iw][V], binput_);
      MD(Type, ainput, [this->n][this->ic][this->ih][this->iw], input);

#pragma omp parallel for collapse(3)
      for_each (_n, this->n) {
      for_each (_ic2, this->ic2) {
      for_each (_ih, this->ih) {
      for_each (_iw, this->iw) {
#pragma omp simd
      for_each (_V, V) {
        abinput[_n][_ic2][_ih][_iw][_V] = ainput[_n][_ic2 * V + _V][_ih][_iw];
      }}}}}
      in = binput_;
    }

    if (weights_as_bfmt_) {
      MD(Type, abweights, [this->oc2][this->ic2][this->kh][this->kw][V][V], bweights_);
      MD(Type, aweights, [this->oc][this->ic][this->kh][this->kw], weights);

#pragma omp parallel for collapse(3)
      for_each (_oc2, this->oc2) {
      for_each (_ic2, this->ic2) {
      for_each (_kh, this->kh) {
      for_each (_kw, this->kw) {
      for_each (_iV, V) {
#pragma omp simd
      for_each (_oV, V) {
        abweights[_oc2][_ic2][_kh][_kw][_iV][_oV]
          = aweights[_oc2 * V + _oV][_ic2 * V + _iV][_kh][_kw];
      }}}}}}
      wei = bweights_;
    }

    (this->*execute_opt_)(out, in, wei, bias);

    if (output_as_bfmt_) {
      MD(Type, aboutput, [this->n][this->oc2][this->oh][this->ow][V], boutput_);
      MD(Type, aoutput, [this->n][this->oc][this->oh][this->ow], output);

#pragma omp parallel for collapse(3)
      for_each (_n, this->n) {
      for_each (_oc2, this->oc2) {
      for_each (_oh, this->oh) {
      for_each (_V, V) {
      for_each (_ow, this->ow) {
        aoutput[_n][_oc2 * V + _V][_oh][_ow] = aboutput[_n][_oc2][_oh][_ow][_V];
      }}}}}
    }
  }
}

} // namespace euler