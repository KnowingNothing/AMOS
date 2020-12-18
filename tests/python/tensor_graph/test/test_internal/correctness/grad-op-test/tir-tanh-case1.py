from tvm import testing
import tvm
import numpy as np 
import torch


dim1 = 8
dim2 = 4
shape_size = [dim1, dim2]
dtype = "float32"

A = tvm.te.placeholder(shape_size, dtype=dtype, name="A", requires_grad=True)
C = tvm.te.compute(A.shape, lambda *args: tvm.tir.tanh(A[args]), "tanh", requires_grad=True)

dC = tvm.te.placeholder(A.shape, dtype=dtype, name="dC")
dA, = tvm.tg.gradient(C, [A], dC)
#dA = tvm.tg.grad_op(A, C, dC)

s = tvm.te.create_schedule(dA.op)

print(tvm.lower(s, [A, dC, dA], simple_mode=True))

func = tvm.build(s, [A, dC, dA], target="llvm")

A_np = np.random.uniform(-10, 10, shape_size).astype("float32")
#elements are all 1
dC_np = np.ones(shape_size).astype("float32")
dA_np = np.zeros(shape_size).astype("float32")

ctx = tvm.context("llvm", 0)
A_tvm = tvm.nd.array(A_np, ctx)
dC_tvm = tvm.nd.array(dC_np, ctx)
dA_tvm = tvm.nd.array(dA_np, ctx)

func(A_tvm, dC_tvm, dA_tvm)

print("dA_tvm", dA_tvm)

# =======>
# compare the results with pytorch
A_torch = torch.tensor(A_np, requires_grad=True)
C_torch = torch.tanh(A_torch)
loss = C_torch.sum()
loss.backward()
print("Pytorch gradient:\n", A_torch.grad.numpy())
testing.assert_allclose(dA_tvm.asnumpy(), A_torch.grad.numpy(), atol=1e-6, rtol=1e-7)
print("Compare with PyTorch success!")


