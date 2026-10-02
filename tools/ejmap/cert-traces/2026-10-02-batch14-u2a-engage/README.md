# 2026-10-02 batch 14: NEOLD U2A with the engage search (10:46-10:49, mains, iLok 0x01130000 / 2 still present)

`--cert-sweep --profile --product "NEOLD U2A"` with the widened engage signature (section 22 of the
driver doc): the 16-position profile sweep read every Peak Reduction position identically (+0.18 dB,
soft saturation above -10 dBFS), the search tried its one candidate (Mode -> 1.0 'Limit', `eq6.`
traces) and read flat again. The record (`fixtures/`, copied to the store) carries the tried list.
45 processes, 0 retried. LEAD: writes land and are not processed.
