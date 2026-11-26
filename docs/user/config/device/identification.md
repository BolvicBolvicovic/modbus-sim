# `(device (vendor-name ...))` and the other identification objects

Set the objects answered by Read Device Identification (FC 43, MEI 14).

## Syntax

```scheme
(vendor-name "Text")
(product-code "Text")
(major-minor-revision "Text")
(vendor-url "Text")
(product-name "Text")
(model-name "Text")
(user-application-name "Text")
```

## Default

`vendor-name "mogbus"`, `product-code "mogbus"`, `major-minor-revision "1.0"`; the others absent

## Valid in

- `(device ...)`

## Behaviour

Each key sets one object, text of 0-244 bytes.

| Key | Object |
|---|---|
| `vendor-name` | 0x00 (mandatory) |
| `product-code` | 0x01 (mandatory) |
| `major-minor-revision` | 0x02 (mandatory) |
| `vendor-url` | 0x03 |
| `product-name` | 0x04 |
| `model-name` | 0x05 |
| `user-application-name` | 0x06 |

Setting any regular object (0x03-0x06) reports regular conformity (`0x82`). `ReadDevId` code
`01` is basic, `02` regular, `03` extended and `04` specific; see
[Modbus support](../../modbus/modbus-overview.md) for the response semantics.

## Examples

```scheme
(device (unit-id 1)
  (vendor-name "Acme")
  (product-code "EM-100")
  (major-minor-revision "2.3")
  (model-name "Energy meter")
  (holding 0))
```

## See also

- [device-id-object](device-id-object.md)
