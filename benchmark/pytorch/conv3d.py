import torch
import numpy as np
import argparse


def conv3d(N, C, D, H, W, K, KD, R, S, stride_d, stride, padding_d, padding, dilation, dtype):
  A_np = np.random.uniform(-10, 10, [N, C, D, H, W]).astype("float32")
  B_np = np.random.uniform(-10, 10, [K, C, KD, R, S]).astype("float32")

  # What's supported by NVIDIA? Refer to https://docs.nvidia.com/cuda/ampere-tuning-guide/index.html

  # What's supported by pytorch? I don't know
  # Please sudo nvprof them!

  torch.backends.cuda.matmul.allow_tf32 = False
  torch.backends.cudnn.allow_tf32 = False

  if dtype == "FP16": # HMMA-16, torch.float16 or torch.half
    A_torch = torch.tensor(A_np).type(torch.float16).cuda()
    B_torch = torch.tensor(B_np).type(torch.float16).cuda()
  elif dtype == "BF16": # HMMA-16, only on NVIDIA A100, torch.bfloat16
    A_torch = torch.tensor(A_np).type(torch.bfloat16).cuda()
    B_torch = torch.tensor(B_np).type(torch.bfloat16).cuda()
  elif dtype == "FP32":
    A_torch = torch.tensor(A_np).type(torch.float32).cuda()
    B_torch = torch.tensor(B_np).type(torch.float32).cuda()
  elif dtype == "TF32": # HMMA-19, NVIDIA A100
    # Please upgrade torch to 1.7; only supported on A100
    # https://pytorch.org/docs/stable/notes/cuda.html#tf32-on-ampere
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    A_torch = torch.tensor(A_np).type(torch.float32).cuda()
    B_torch = torch.tensor(B_np).type(torch.float32).cuda()
  elif dtype == "INT8": # IMMA, but pytorch has no support for INT8 GEMM
    A_torch = torch.tensor(A_np).type(torch.int8).cuda()
    B_torch = torch.tensor(B_np).type(torch.int8).cuda()
  # Pytorch has no int4 type
  elif dtype == "BOOL": # BMMA, but pytorch has no support for BOOL GEMM
    A_torch = torch.tensor(A_np).type(torch.bool).cuda()
    B_torch = torch.tensor(B_np).type(torch.bool).cuda()
  elif dtype == "FP64": # DMMA(FP64), only supported on A100
    A_torch = torch.tensor(A_np).type(torch.float64).cuda()
    B_torch = torch.tensor(B_np).type(torch.float64).cuda()
  else:
    assert False, "wrong type: " + dtype

  global RUN_NUMBER
  number, repeats = RUN_NUMBER

  for i in range(repeats):
      time_record = []
      for j in range(number):
          torch.cuda.synchronize()
          start = torch.cuda.Event(enable_timing=True)
          end = torch.cuda.Event(enable_timing=True)
          start.record()

          C_torch = torch.nn.functional.conv3d(A_torch, B_torch, bias=None, stride=(stride_d, stride, stride), 
                  padding=(padding_d, padding, padding), dilation=dilation)

          end.record()
          torch.cuda.synchronize()
          total = start.elapsed_time(end)
          time_record.append(total)
      if i == repeats - 1:
        mean_cost = np.mean(time_record)
  #print("conv3d, dtype = %s, A: %s, B: %s, C:%s" % (dtype, A_torch.dtype, B_torch.dtype, C_torch.dtype))
  print(",".join(map(str, [N, C, D, H, W, K, KD, R, S, stride_d, stride, padding_d, padding, dilation, dtype, mean_cost])))


_ = None
L = 8

#  (  N,   C,     L,   H,   W,   K,   D,   R,   S, stride_d, stride, padding_d, padding, dilation)
res3d_18_shapes = [
    ( _,   3,     L, 112, 112,  64,   1,   3,   3,        3,      7,         1,       3,        1), # stem

    ( _,  64,     L,  56,  56,  64,   3,   3,   3,        1,      1,         1,       1,        1), # layer1 x 4

    ( _,  64,     L,  56,  56, 128,   1,   1,   1,        2,      2,         0,       0,        1), # layer2 downsample
    
    ( _,  64,     L,  56,  56, 128,   3,   3,   3,        2,      2,         1,       1,        1), # layer2
    ( _, 128,  L//2,  28,  28, 128,   3,   3,   3,        1,      1,         1,       1,        1), # layer2 x 3

    ( _, 128,  L//2,  28,  28, 256,   1,   1,   1,        2,      2,         0,       0,        1), # layer3 downsample
    ( _, 128,  L//2,  28,  28, 256,   3,   3,   3,        2,      2,         1,       1,        1), # layer3
    ( _, 256,  L//4,  14,  14, 256,   3,   3,   3,        1,      1,         1,       1,        1), # layer3 x 3

    ( _, 256,  L//4,  14,  14, 512,   1,   1,   1,        2,      2,         0,       0,        1), # layer4 downsample
    ( _, 256,  L//4,  14,  14, 512,   3,   3,   3,        2,      2,         1,       1,        1), # layer4
    ( _, 256,  L//8,   7,   7, 512,   3,   3,   3,        1,      1,         1,       1,        1), # layer4 x 3
]

example_text = """
    example:
        python conv3d.py --batch 16 --enable_cudnn --number 5 --repeats 5 --begin 0 --num 11 --dtype FP16
        python conv3d.py --batch 8 --number 10 --repeats 10 --begin 0 --num 5 --dtype TF32
"""

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog="base_maker",
        description="template maker",
        epilog=example_text,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--batch", type=int, default=1)
    parser.add_argument("--enable_cudnn", action="store_true")
    parser.add_argument("--number", type=int, default=10)
    parser.add_argument("--repeats", type=int, default=10)
    parser.add_argument("--begin", type=int, choices=list(range(len(res3d_18_shapes))), default=0)
    parser.add_argument(
        "--num",
        type=int,
        choices=list(range(1, len(res3d_18_shapes) + 1)),
        default=len(res3d_18_shapes),
    )
    parser.add_argument(
        "--dtype",
        type=str,
        choices=["FP16", "FP32", "TF32", "FP64", "BF16", "INT8", "BOOL"],
        default="FP16",
    )

    args = parser.parse_args()

    if args.enable_cudnn:
        assert torch.backends.cudnn.is_available()
        torch.backends.cudnn.enabled = True
    else:
        torch.backends.cudnn.enabled = False

    batch = args.batch
    beg = args.begin
    num = args.num

    RUN_NUMBER = (args.number, args.repeats)

    print("N, C, D, H, W, K, KD, R, S, stride_d, stride, padding_d, padding, dilation, type, cost")
    costs = []
    for i, shape in enumerate(res3d_18_shapes[beg:beg+num]):
        (_, C, D, H, W, K, KD, R, S, stride_d, stride, padding_d, padding, dilation) = shape
        N = batch
        conv3d(N, C, D, H, W, K, KD, R, S, stride_d, stride, padding_d, padding, dilation, args.dtype)

    print("cudnn: %s" % ("enabled" if torch.backends.cudnn.enabled else "disabled"))
