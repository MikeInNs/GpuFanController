# Protocol assets

`PROTOCOL.md` is the canonical serial contract. Host encoding lives in
`src/protocol`; firmware encoding lives in `firmware/FanControllerFirmware/src`.
Wire changes must update both implementations, documentation, and test vectors.
The C++20 host code is separate from the Arduino AVR toolchain.

`tests/protocol_tests.cpp` checks the CCITT-FALSE reference CRC and the known v2
temperature frame `A5 5A 02 02 34 12 05 01 6F 02 00 00 F1 ED`.
