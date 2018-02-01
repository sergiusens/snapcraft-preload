# Use

Add this as a part to your `snapcraft.yaml`:

```yaml
parts:
    snapcraft-preload:
        source: https://github.com/sergiusens/snapcraft-preload.git
        plugin: cmake
        build-packages:
            - gcc-multilib
            - g++-multilib

    # We keep the 'preload' part to be backward compatible
    preload:
      plugin: nil
      install: |
        mkdir -p $SNAPCRAFT_PART_INSTALL/bin
        ln -sv snapcraft-preload $SNAPCRAFT_PART_INSTALL/bin/preload
after: [snapcraft-preload]
```

And precede your `apps` entry like this:

```yaml
apps:
    app-name:
        command: snapcraft-preload <binary>
```
