# RAW user data

Kemu opens `avd/kemu.avd/userdata-qemu.img` directly as a writable RAW block
device. Userdata uses ext4 with Android encryption enabled.
`system-img/userdata.img` remains the initialization template.

New Kemu instances default to `128G` (128 GiB). Existing configured capacities
are preserved. Creating a sparse data image does not require free host space
equal to its virtual capacity. Startup does not enforce a virtual-capacity
space check or a minimum-free-space threshold on the host.
Image creation and actual writes still report filesystem or block-device errors.
Exhausting host storage can interrupt writes and damage guest data; reaching
the guest filesystem's capacity produces its normal out-of-space behavior.

New data images are copied or formatted as sparse files and expanded to the
configured virtual capacity. Initialization happens in `userdata-qemu.img.raw-init`;
only a completed image replaces the live path. Windows requires a filesystem
supporting sparse files, such as NTFS.

Image packaging copies the original AOSP userdata template without changing
its inode density. Existing filesystems retain their original inode layout.

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
- Confirmed expansion of encrypted userdata through Settings > Performance.
- System-image version changes without resetting the encryption-key overlay.

Snapshots, snapshot imports and read-only shared instances are disabled for RAW
userdata. Live snapshot save/load requests are rejected. Legacy
`userdata.useQcow2` configuration is ignored; `-qcow2-for-userdata` is rejected.

Settings > Performance shows the data image's allocated host space and virtual
capacity. Expand disk asks for a larger capacity and confirmation to restart.
The launcher waits for the old emulator to exit, extends the sparse RAW file
under exclusive access, and updates the AVD capacity. Android recreates its
encryption mapping for the larger block device on boot, then Kemu runs Android's
resize2fs on the mounted data filesystem. The expansion request remains until
the guest filesystem size is verified; a later launch can retry an interrupted
operation. The Android image must provide root shell access and resize2fs.

Shrinking remains unsupported. Direct engine startup still rejects host-side
resizing of an encrypted image; use the confirmed Kemu expansion flow.

Sparse allocation does not automatically reclaim deleted guest files.
Host TRIM/hole-punching support is a separate requirement. Copy and backup tools
must preserve sparse files to retain their reduced physical allocation.
