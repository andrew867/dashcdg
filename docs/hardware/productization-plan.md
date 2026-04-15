# Hardware Productization Plan

## EVT

- first board bring-up
- Wi-Fi and display signal integrity
- multicast/broadcast receive and late-join bootstrap sanity
- battery charging and protection validation
- boot, recovery, and firmware flashing workflows

## DVT

- long-duration packet-loss soak
- synchronized live audio/CD+G soak against the desktop proof transport
- mixed network-topology validation against both multicast and broadcast desktop sender setups
- battery charge/discharge endurance
- brownout and reset recovery
- ESD and connector abuse checks
- OTA interruption and rollback validation

## PVT

- factory test firmware
- serial number and provisioning flow
- calibration and traceability records
- acceptance criteria for display, radio, power, and controls

## Required outputs

- board bring-up checklist
- fixture requirements
- operator test instructions
- field service and firmware recovery procedure
- explicit transport-version and provisioning compatibility notes for mixed desktop/embedded validation
