; Point actions: statements that run when a Modbus client reads or writes a point.
; A request counter that wraps, a value derived from another point on every read, and a
; coil a client writes to reset the counter.
; Try: mbpoll -a 1 -t 4 -r 1 -c 3 -p 1502 127.0.0.1   (the first register counts reads)
(server
  (mode tcp)
  (port 1502))

; A reusable action. Params are read-only and can be overridden per use. The count lives in
; the point itself, so a write to the point (or the reset coil below) restarts it.
(define-action rolling-counter
  (param step 1)
  (param limit 65535)
  (set! self (if (>= (+ self step) limit) 0 (+ self step))))

(device
  (name "read-actions")
  (unit-id 1)

  ; (holding 0) counts by 5 and wraps at 100.
  (holding 0 (static 0)
    (on-read (rolling-counter (step 5) (limit 100))))

  ; Inline body: twice the counter, computed when the register is read.
  (holding 1 (static 0)
    (on-read (set! self (* 2 (holding 0)))))

  ; Time since this point was last read, in milliseconds (0 on the first read).
  (holding 2 (static 0)
    (on-read (set! self (min 65535 (elapsed-ms)))))

  ; Writing 1 to this coil resets the counter, then clears the coil.
  (coil 0 (static #f)
    (on-write
      (when self
        (set! (holding 0) 0)
        (set! self #f)))))
