"""Retarget this repository's static Ultralytics YOLOv8 ONNX export.

The original export has the detection grid and DFL reshape counts folded into
constants. This tool verifies those constants against the original input size
before regenerating them for a new square input size.
"""

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper


def grid(size: int) -> tuple[np.ndarray, np.ndarray]:
    xs, ys, strides = [], [], []
    for stride in (8, 16, 32):
        side = size // stride
        y, x = np.meshgrid(
            np.arange(side, dtype=np.float32) + 0.5,
            np.arange(side, dtype=np.float32) + 0.5,
            indexing="ij",
        )
        xs.append(x.reshape(-1))
        ys.append(y.reshape(-1))
        strides.append(np.full(side * side, stride, dtype=np.float32))
    anchors = np.stack((np.concatenate(xs), np.concatenate(ys)), axis=0)[None]
    return anchors, np.concatenate(strides)[None]


def retarget(source: Path, destination: Path, size: int) -> None:
    if size <= 0 or size % 32:
        raise ValueError("input size must be a positive multiple of 32")
    if destination.exists():
        raise FileExistsError(destination)

    model = onnx.load(source, load_external_data=False)
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise ValueError("expected one input and one output")
    input_dims = [dim.dim_value for dim in model.graph.input[0].type.tensor_type.shape.dim]
    output_dims = [dim.dim_value for dim in model.graph.output[0].type.tensor_type.shape.dim]
    if len(input_dims) != 4 or input_dims[:2] != [1, 3] or input_dims[2] != input_dims[3]:
        raise ValueError(f"unexpected input shape: {input_dims}")
    original_size = input_dims[2]
    original_anchors, original_strides = grid(original_size)
    if output_dims != [1, 9, original_anchors.shape[2]]:
        raise ValueError(f"unexpected output shape: {output_dims}")

    constants = {node.name: node for node in model.graph.node if node.op_type == "Constant"}
    expected = {
        "/model.22/dfl/Constant": np.array([1, 4, 16, output_dims[2]], dtype=np.int64),
        "/model.22/dfl/Constant_1": np.array([1, 4, output_dims[2]], dtype=np.int64),
        "/model.22/Constant_12": original_anchors,
        "/model.22/Constant_13": original_anchors,
        "/model.22/Constant_15": original_strides,
    }
    for name, value in expected.items():
        node = constants.get(name)
        if node is None or len(node.attribute) != 1 or node.attribute[0].name != "value":
            raise ValueError(f"missing expected graph constant: {name}")
        actual = numpy_helper.to_array(node.attribute[0].t)
        if not np.array_equal(actual, value):
            raise ValueError(f"graph constant differs from YOLOv8 grid: {name}")

    anchors, strides = grid(size)
    count = anchors.shape[2]
    replacement = {
        "/model.22/dfl/Constant": np.array([1, 4, 16, count], dtype=np.int64),
        "/model.22/dfl/Constant_1": np.array([1, 4, count], dtype=np.int64),
        "/model.22/Constant_12": anchors,
        "/model.22/Constant_13": anchors,
        "/model.22/Constant_15": strides,
    }
    for name, value in replacement.items():
        constants[name].attribute[0].t.CopyFrom(numpy_helper.from_array(value))

    model.graph.input[0].type.tensor_type.shape.dim[2].dim_value = size
    model.graph.input[0].type.tensor_type.shape.dim[3].dim_value = size
    model.graph.output[0].type.tensor_type.shape.dim[2].dim_value = count
    for entry in model.metadata_props:
        if entry.key == "imgsz":
            entry.value = f"[{size}, {size}]"

    onnx.checker.check_model(model)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    try:
        onnx.save(model, temporary)
        temporary.replace(destination)
    finally:
        if temporary.exists():
            temporary.unlink()
    print(f"Saved {destination} — input [1,3,{size},{size}], output [1,9,{count}]")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("size", type=int)
    args = parser.parse_args()
    retarget(args.source, args.destination, args.size)
