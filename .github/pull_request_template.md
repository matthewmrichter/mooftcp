## What and why

## Checklist

- [ ] Host tests pass in both configurations (`ctest` in `build/` and in `build-lite/` with `-DMT_NO_DHCP=ON`)
- [ ] `scripts/gate-check.sh` passes if Mac code changed
- [ ] Tests added or updated for new behavior
- [ ] Resident memory impact stated (driver size change, if any)
- [ ] No Apple software, ROMs, disk images or built binaries are included
