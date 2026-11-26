# `mogbus_frames_rejected_total`

Frames rejected before they reached a device.

## Syntax

```text
mogbus_frames_rejected_total
```

## Valid in

- `counter`
- always present

## Behaviour

- Counts a bad CRC or LRC, a malformed frame and an oversized frame.

## Examples

```text
mogbus_frames_rejected_total 0
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
