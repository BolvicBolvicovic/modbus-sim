# `mogbus_response_latency_seconds`

Time from a request to its scheduled response.

## Syntax

```text
mogbus_response_latency_seconds_bucket{le="0.001"}
```

## Valid in

- `histogram`
- always present

## Behaviour

- It mainly shows injected `delay` faults: the time is the scheduled delay, not the network round trip.
- Buckets run from 1 ms to 10 s (`le` 0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, +Inf), with the usual `_sum` and `_count` series.

## Examples

```text
mogbus_response_latency_seconds_bucket{le="0.001"} 2
mogbus_response_latency_seconds_bucket{le="+Inf"} 2
mogbus_response_latency_seconds_sum 0
mogbus_response_latency_seconds_count 2
```

## See also

- [Metrics endpoint](../http-endpoint.md)
- [Metrics](metrics.md)
