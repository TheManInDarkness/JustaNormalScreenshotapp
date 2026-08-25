# PP-OCR models + onnxruntime

The primary OCR engine. Windows' built-in `Windows.Media.Ocr` remains as the
automatic fallback when these files are missing.

## Files

- `ch_PP-OCRv4_det_infer.onnx` (~4.7 MB) - text detection (DBNet)
- `ch_PP-OCRv4_rec_infer.onnx` (~10.9 MB) - text recognition (SVTR, CTC head).
  The recognition charset is embedded in the model's custom metadata key
  `character` (6623 entries); the decoder prepends `blank` at index 0 and
  appends ` ` at the end, exactly like PaddleOCR's `CTCLabelDecode`.
- `../onnxruntime/onnxruntime.dll` (~11.5 MB) - MIT-licensed runtime from
  https://github.com/microsoft/onnxruntime (v1.20.1, win-x64). Loaded at run
  time with `LoadLibraryW`; the app starts fine without it and falls back to
  the Windows engine.

## Provenance and licensing

Both models are the ONNX conversions of PaddleOCR's PP-OCRv4 mobile models
distributed by the RapidOCR project (https://github.com/RapidAI/RapidOCR,
Apache-2.0); PaddleOCR itself is Apache-2.0 (c) PaddlePaddle/Baidu. The
detection/recognition pipeline parameters (resize limits, thresholds,
unclip ratio, CTC decoding) mirror RapidOCR's reference implementation.
