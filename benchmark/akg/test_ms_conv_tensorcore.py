# Copyright 2021 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License
import numpy as np
# from akg.ops.math_gpu.tensorcore_conv import conv_tc
from akg.ops.nn.gpu.tensorcore_conv import TensorcoreConv
from tests.common.gen_random import random_gaussian
from akg.utils import kernel_exec as utils
from akg.utils.result_analysis import target_profiling
from akg.utils.format_transform import to_tvm_nd_array


def has_pad(padding):
    p_l, p_r, p_t, p_b = padding
    return not(p_l == 0 and p_r == 0 and p_t == 0 and p_b == 0)


def gen_data_im2col(shape_data, shape_filter, stride, padding, dilation, dtype, out_dtype):
    support_list = {"float16": np.float16, "float32": np.float32}
    n, h, w, c = shape_data
    out_c, kh, kw, c = shape_filter
    s_h, s_w = stride
    d_h, d_w = dilation
    p_l, p_r, p_t, p_b = padding
    out_h = (h + p_t + p_b - kh) // s_h + 1
    out_w = (w + p_l + p_r - kw) // s_w + 1

    out_shape = (n, out_h, out_w, out_c)
    shape_data_pad = (n, h + p_t + p_b, w + p_l + p_r, c)

    data = random_gaussian(shape_data, miu=1,
                           sigma=0.1).astype(support_list[dtype])
    filter_ = random_gaussian(shape_filter, miu=1,
                              sigma=0.1).astype(support_list[dtype])

    """
    initialization data with padding
    """
    data_pad = np.zeros(shape_data_pad).astype(support_list[dtype])
    if has_pad(padding):
        data_pad[:, p_t:p_t+h, p_l:p_l+w, :] = data
    else:
        data_pad = data

    whd = (kh - 1) * d_h + 1
    wwd = (kw - 1) * d_w + 1
    expect = np.zeros(out_shape).astype(support_list[out_dtype])
    for i in range(out_h):
        for j in range(out_w):
            for f in range(out_c):
                expect[:, i, j, f] = np.sum(
                    data_pad[:, i*s_h:i*s_h+whd:d_h, j*s_w:j*s_w+wwd:d_w, :].astype("float32") *
                    filter_[f, :, :, :].astype("float32"),
                    axis=(1, 2, 3)
                )

    output = np.full(expect.shape, np.nan, out_dtype)
    print("expect shape is ", np.shape(expect))
    return data, filter_, output, expect


def test_ms_conv_tc(shape_data, shape_filter, stride, padding, dilation, dtype, out_dtype="float32", poly_sch=True, attrs=None):
    op_attrs = [stride, padding, dilation, out_dtype]
    default_attrs = {"target": "cuda", "enable_auto_fuse": False}
    # default_attrs.update({"pragma_enable_matmul": True, "pragma_enable_conv_tensor_core": True})
    # default_attrs.update({"pragma_enable_matmul":True})
    default_attrs.update({"pragma_enable_conv_tensor_core": True})
    if attrs:
        default_attrs.update(attrs)

    data, weight, output, expect = gen_data_im2col(
        shape_data, shape_filter, stride, padding, dilation, dtype, out_dtype)

    if poly_sch:
        mod = utils.op_build_test(TensorcoreConv, (data.shape, weight.shape), (
            dtype, dtype), op_attrs=op_attrs, attrs=default_attrs, kernel_name="conv_tc_auto")
    args = (data, weight, output)
    output = utils.mod_launch(mod, args, expect=expect)
    res = np.allclose(output, expect, rtol=5e-3, atol=1.e-8)
    print("Test {}".format("Pass" if res else "Fail"))
    if not res:
        print("Error cuda:===================================")
        print(mod.imported_modules[0].get_source())
        raise AssertionError("Test fail")

    data, weight, output, expect = to_tvm_nd_array(
        [data, weight, output, expect])
    target_profiling(mod, data, weight, output, expect, repeat_time=100)

if __name__=="__main__":
    #shape_data: n,h,w,c
    #shape_filter: outc, kh, kw, c
    #stride = []: s_h, s_w
    #padding = []: p_l, p_r, p_t, p_b
    #dilation = []: d_h, d_w
    batches = [1]
    dtype = "float16"
    for n in batches:
        print("n = ", n)
        print("=========================================================================================================")
        #               (n,h, w, c),(out_c,kh,kw,c),(stride),(pad), (dilation)
        test_ms_conv_tc((n,224,224,3), (64,7,7,3),  (2,2), (3,3,3,3), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        test_ms_conv_tc((n,56, 56,64), (64,3,3,64), (1,1), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        try:
            test_ms_conv_tc((n,56, 56,64), (64,1,1,64), (1,1), (0,0,0,0), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        except Exception as e:
            print("(n,56, 56,64), (64,1,1,64) fails ", e)
        test_ms_conv_tc((n,56, 56,64), (128,3,3,64), (2,2), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        try:
            test_ms_conv_tc((n,56, 56,64), (128,1,1,64), (2,2), (0,0,0,0), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        except Exception as e:
            print("(n,56, 56,64), (128,1,1,64) fails ", e)
        test_ms_conv_tc((n,28,28,128),(128,3,3,128), (1,1), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        test_ms_conv_tc((n,28,28,128),(256,3,3,128), (2,2), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        try:
            test_ms_conv_tc((n,28,28,128),(256,1,1,128), (2,2), (0,0,0,0), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        except Exception as e:
            print("(n,28,28,128),(256,1,1,128) fails ", e)
        test_ms_conv_tc((n,14,14,256),(256,3,3,256), (1,1), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        test_ms_conv_tc((n,14,14,256),(512,3,3,256), (2,2), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        try:
            test_ms_conv_tc((n,14,14,256),(512,1,1,256), (2,2), (0,0,0,0), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
        except Exception as e:
            print("(n,14,14,256),(512,1,1,256) fails ", e)
        test_ms_conv_tc((n, 7, 7,512),(512,3,3,512), (1,1), (1,1,1,1), (1,1), dtype, out_dtype="float32", poly_sch=True, attrs=None)
