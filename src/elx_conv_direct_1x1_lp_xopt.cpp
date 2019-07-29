#include <string.h>
#include "el_parallel.hpp"
#include "elx_conv_direct_1x1_lp.hpp"

// XOPT
// kernel options:
//   - a: CCC, s1
//   - b: CCD, s1
//   - c: DCD: s1
// fusion:  same as winograd
// dup:     same as winograd
//
// ------+-----+--------+-----+--------------------------------------
//       | ker | fusion | dup |             notes
// ------+-----+--------+-----+--------------------------------------
//  c060 |  c  |   t+o  |  -  | blocked/nhwc, Tr, Or, stride=1
// ------+-----+--------+-----+--------------------------------------
//  b061 |  b  |   t+o  |  I  | blocked, stride>=1, oh = wt*T
// ------+-----+--------+-----+--------------------------------------
//

namespace euler {

Template_elx_conv_direct_1x1_lp_t
void Instance_elx_conv_direct_1x1_lp_t::__execute_c160(
    OutputType *output, InputType *input, WeightsType *weights, BiasType *bias)
{
  // weights: oc4*, oc3, O2, ic4*, ic3, I2, V, V
  // input:   t3*, ic4*, ic3, I2, t2*, T(Tr), V
  // output:  t3*, oc4*, oc3, O2, t2*, T(Tr), V

  if (is_first_run_) {
    trans_weights_s8_oc(weights_scale_, tweights_s8_, weights, bias);
    MD2(TscaleType, ainput_scale, input_scale_, 2, this->T);
    iter_each (_T, this->T) {
      md2(ainput_scale, 0, _T) = this->input_quant_S;
      md2(ainput_scale, 1, _T) = this->input_quant_z;
    }
  }

  parallel_for<4, 2>(mthr_, [&](int _t3, int _oc4, int _ic4, int _t2) {
    auto ithr = omp_get_thread_num();
    // input
    MD2(uint8_t, ainput_blocked, input,
        this->t3, this->ih * this->iw * this->IC);
    MD2(uint8_t, ainput_nhwc, input,
        this->t3, this->ih * this->iw * this->ic);
    // output
    MD2(OutputType, aoutput_blocked, output,
        this->t3, this->oh * this->ow * this->OC);
    MD2(OutputType, aoutput_nhwc, output,
        this->t3, this->oh * this->ow * this->oc);
    // toutput
    MD2(ToutputType, atoutput_blocked, toutput_,
        this->t3, this->oh * this->ow * this->OC);
    MD2(ToutputType, atoutput_nhwc, toutput_,
        this->t3, this->oh * this->ow * this->oc);
    MD2(ToutputType, atoutput_opt, toutput_,
        mthr_, this->oc3 * this->O2 * V * this->oh * this->ow);
    MD2(BiasType, abias, bias, this->oc4, this->oc3 * this->O2 * V);

    MD2(TscaleType, ainput_scale, input_scale_, 2, this->T);
    MD3(int8_t, atweights_s8, tweights_s8_, this->oc4, this->ic4,
        this->oc3 * this->ic3 * this->O2 * this->I2 * V * V);
    MD2(TscaleType, aweights_scale, weights_scale_,
        this->oc4, this->oc3 * 2 * this->O2 * V);

    auto ain = this->input_fmt == nhwc
             ? &md2(ainput_nhwc, _t3, 0) : &md2(ainput_blocked, _t3, 0);
    auto aout = this->output_fmt == nhwc
             ? &md2(aoutput_nhwc, _t3, 0) : &md2(aoutput_blocked, _t3, 0);
    auto atout = toutput_opt_
             ? &md2(atoutput_opt, ithr, 0) : this->output_fmt == nhwc
               ? &md2(atoutput_nhwc, _t3, 0) : &md2(atoutput_blocked, _t3, 0);

    gemm_c160(atout, aout, ain,
        &md3(atweights_s8, _oc4, _ic4, 0),
        &md2(ainput_scale, 0, 0),
        &md2(aweights_scale, _oc4, 0),
        &md2(abias, _oc4, 0),
        _ic4, _oc4, _t2);
  }, this->t3, this->oc4, this->ic4, this->t2);

  if (inference_acc_)
    is_first_run_ = false;
}

Template_elx_conv_direct_1x1_lp_t
void Instance_elx_conv_direct_1x1_lp_t::__execute_b161(
    OutputType *output, InputType *input, WeightsType *weights, BiasType *bias)
{
  // weights: oc4*, oc3, O2, ic4*, ic3, I2, V, V
  // input:   t3*, ic4*, ic3, I2, t2*, T(Tr), V
  // output:  t3*, oc4*, oc3, O2, t2*, T(Tr), V

  if (is_first_run_) {
    trans_weights_s8_oc(weights_scale_, tweights_s8_, weights, bias);
    MD2(TscaleType, ainput_scale, input_scale_, 2, this->T);
    iter_each (_T, this->T) {
      md2(ainput_scale, 0, _T) = this->input_quant_S;
      md2(ainput_scale, 1, _T) = this->input_quant_z;
    }
  }

  parallel_for<5, 2>(mthr_, [&](int _t3, int _oc4, int _ic4, int _ht, int _wt) {
    auto ithr = omp_get_thread_num();
    MD7(uint8_t, ainput, input, this->t3, this->ic4, this->ic3 * this->I2,
        this->ht, this->hs, this->wt, this->T * this->ws * V);
    MD6(ToutputType, atoutput, toutput_,
        this->t3, this->oc4, this->oc3 * this->O2, this->ht, this->wt, this->T * V);
    MD5(ToutputType, atoutput_opt, toutput_,
        mthr_, this->oc3 * this->O2, this->ht, this->wt, this->T * V);
    MD2(OutputType, aoutput, output, this->t3, this->OC * this->oh * this->ow);
    MD5(OutputType, aoutput2, &md2(aoutput, _t3, 0), this->oc4,
        this->oc3 * this->O2, this->ht, this->wt, this->T * V);
    MD2(BiasType, abias, bias, this->oc4, this->oc3 * this->O2 * V);
    MD2(TscaleType, ainput_scale, input_scale_, 2, this->T);
    MD3(int8_t, atweights_s8, tweights_s8_, this->oc4, this->ic4,
        this->oc3 * this->ic3 * this->O2 * this->I2 * V * V);
    MD2(TscaleType, aweights_scale, weights_scale_,
        this->oc4, this->oc3 * 2 * this->O2 * V);
    auto atout = toutput_opt_ ? &md5(atoutput_opt, ithr, 0, _ht, _wt, 0)
                              : &md6(atoutput, _t3, _oc4, 0, _ht, _wt, 0);

    gemm_b161(atout,
        &md5(aoutput2, _oc4, 0, _ht, _wt, 0),
        &md7(ainput, _t3, _ic4, 0, _ht, 0, _wt, 0),
        &md3(atweights_s8, _oc4, _ic4, 0),
        &md2(ainput_scale, 0, 0),
        &md2(aweights_scale, _oc4, 0),
        &md2(abias, _oc4, 0),
        _ic4);
  }, this->t3, this->oc4, this->ic4, this->ht, this->wt);

  if (inference_acc_)
    is_first_run_ = false;
}

Template_elx_conv_direct_1x1_lp_t
void Instance_elx_conv_direct_1x1_lp_t::execute(
    void *output, void *input, void *weights, void *bias)
{
  set_trans_buffers();

  (this->*execute_opt_)((OutputType *)output,
      (InputType *)input, (WeightsType *)weights, (BiasType *)bias);
}

} // namespace euler
