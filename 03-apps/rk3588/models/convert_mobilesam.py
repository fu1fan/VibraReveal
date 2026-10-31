#!/usr/bin/env python3
"""Convert official 448px MobileSAM ONNX pair to RK3588 FP16; write provenance.
Run inside an RKNN-Toolkit2 environment. Inputs must come from Rockchip Model Zoo.
No weights are redistributed in this repository.
"""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
from rknn.api import RKNN


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--encoder', required=True, type=Path)
    parser.add_argument('--decoder', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--prompt-slots', type=int, default=8)
    args = parser.parse_args()
    if not 2 <= args.prompt_slots <= 64:
        parser.error('prompt-slots must be 2..64')
    args.output_dir.mkdir(parents=True, exist_ok=False)
    manifest = {'target': 'rk3588', 'quantized': False, 'encoder_size': 448,
                'prompt_slots': args.prompt_slots,
                'toolkit_version': importlib.metadata.version('rknn-toolkit2'),
                'upstream': 'https://github.com/airockchip/rknn_model_zoo/tree/main/examples/mobilesam',
                'models': {}}
    for role, source in [('encoder', args.encoder), ('decoder', args.decoder)]:
        output = args.output_dir / f'mobilesam_{role}.rknn'
        model = RKNN(verbose=False)
        try:
            config = {'target_platform': 'rk3588'}
            if role == 'encoder':
                config.update(mean_values=[[123.675, 116.28, 103.53]],
                              std_values=[[58.395, 57.12, 57.375]])
            load = {'model': str(source)}
            if role == 'decoder':
                load.update(inputs=['image_embeddings', 'point_coords', 'point_labels',
                                    'mask_input', 'has_mask_input'],
                            input_size_list=[[1, 256, 28, 28], [1, args.prompt_slots, 2],
                                             [1, args.prompt_slots], [1, 1, 112, 112], [1]],
                            outputs=['iou_predictions', 'low_res_masks'])
            for name, action in [('config', lambda: model.config(**config)),
                                 ('load', lambda: model.load_onnx(**load)),
                                 ('build', lambda: model.build(do_quantization=False)),
                                 ('export', lambda: model.export_rknn(str(output)))]:
                code = action()
                if code not in (None, 0):
                    raise RuntimeError(f'{role} {name} failed: {code}')
        finally:
            model.release()
        manifest['models'][role] = {'source': source.name, 'onnx_sha256': digest(source),
                                    'file': output.name, 'rknn_sha256': digest(output)}
    (args.output_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
