# RAW user data

Kemu opens `avd/kemu.avd/userdata-qemu.img` directly as a writable RAW block
device. The Android filesystem and encryption settings are unchanged.
`system-img/userdata.img` remains the initialization template.

New data images are copied or formatted as sparse files and expanded to the
configured virtual capacity. Initialization happens in `userdata-qemu.img.raw-init`;
only a completed image replaces the live path. Windows requires a filesystem
supporting sparse files, such as NTFS.

## Existing instances

An existing `userdata-qemu.img.qcow2` or a QCOW2-formatted data image causes startup
to fail instead of silently opening an outdated base image. Preserve a stopped
AVD before migrating. Convert the complete current backing chain to a separate
RAW image, compare the virtual disk contents, then replace the data image and
remove the old overlay. Keep the matching encryption-key image and its overlay.

Alternatively, explicitly reset the instance with the engine's `-wipe-data`
option. This discards its user data and recreates it from the initialization
template. Kemu does not automatically reset existing user data.

## Supported operations

- Normal cold boots, application data, Android encryption and exclusive access.
- Sparse RAW initialization, including trailing zero ranges.
- Growth of an existing unencrypted ext4 image.
- System-image version changes without resetting the encryption-key overlay.

Snapshots, snapshot imports and read-only shared instances are disabled for RAW
userdata. Live snapshot save/load requests are rejected. Legacy
`userdata.useQcow2` configuration is ignored; `-qcow2-for-userdata` is rejected.

Shrinking an existing image and host-side growth of encrypted userdata are
rejected before changing its size. Retain its current capacity or explicitly
reset it at the desired capacity. Supporting encrypted growth requires resizing
the guest's encryption mapping and filesystem together.

Sparse allocation does not automatically reclaim deleted guest files.
Host TRIM/hole-punching support is a separate requirement. Copy and backup tools
must preserve sparse files to retain their reduced physical allocation.
