# Captures

Raw logic analyzer captures in sigrok format (`.sr`), open them in [PulseView](https://sigrok.org/wiki/PulseView) or decode them with [`tools/decode_capture.py`](../tools/decode_capture.py).

| File | What | Settings |
| --- | --- | --- |
| `govee-controller-red.sr` | The original Govee controller box (R2A80) driving the strip, solid red at 100% in the Govee app. 300 ms, 14 full frames. | 24 MHz, D0 = data (A10), D1 = A3, D2 = A2 |

```sh
python3 tools/decode_capture.py --file captures/govee-controller-red.sr
```

```
high pulses (ns: count): [(294, 57658), (924, 23008), (336, 20979), (966, 14375)]
bits per frame: {8100: 14}
 90 x [11]   82  [11]65535  [11]    0  [11]    0  [11]    0   (G R B CW WW)
```

D1 and D2 stay high for the whole capture.
