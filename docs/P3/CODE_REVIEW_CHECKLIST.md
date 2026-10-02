# Phase3 Code Review Checklist


## 1. Architecture Boundary


[ ] Runtime only manages lifecycle and health


[ ] No camera implementation


[ ] No AI inference logic


[ ] No MCU protocol logic


[ ] No service business leakage



---

# 2. Device State Model


[ ] All states implemented:


BOOTING


READY


RUNNING


WARNING


ERROR


RECOVERING


OFFLINE



[ ] Every transition has explicit trigger


[ ] No hidden state change



---

# 3. State Manager


Review:


[ ] Single ownership of device state


[ ] Thread safe


[ ] Deterministic transition


[ ] Reason recorded


[ ] Timestamp recorded



Reject:


global uncontrolled state



---

# 4. Aggregation


Review:


[ ] Service criticality supported


[ ] HIGH/MEDIUM/LOW distinction


[ ] Priority rules match document


[ ] Recovery recalculates state



---

# 5. Event System


Review:


[ ] Events have clear producer


[ ] Events have clear consumer


[ ] No circular dependency


[ ] No external message system



---

# 6. IPC


Review:


[ ] Unix Domain Socket maintained


[ ] Phase2 commands unchanged


[ ] New commands documented


[ ] Error handling complete



---

# 7. Monitor Integration


Review:


[ ] Monitor reports facts only


[ ] StateManager decides state


[ ] Threshold configurable



---

# 8. Persistence


Review:


[ ] No database


[ ] No history storage


[ ] Only diagnostic snapshot allowed



---

# 9. Testing


Required:


[ ] Boot flow


[ ] Service failure


[ ] Recovery


[ ] Aggregation


[ ] Heartbeat timeout


[ ] IPC query


[ ] IPC subscription



---

# 10. Phase4 Compatibility


Check:


[ ] Recovery interface reserved


[ ] State transition supports recovery


[ ] Event model supports recovery events