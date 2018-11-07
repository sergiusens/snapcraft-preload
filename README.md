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
```

And precede your `apps` entry like this:

```yaml
apps:
    app-name:
        command: bin/snapcraft-preload $SNAP/<binary>
```
