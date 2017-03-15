# Use

Add this as a part to your `snapcraft.yaml`:

```yaml
parts:
    snapcraft-preload:
        source: https://github.com/sergiusens/snapcraft-preload.git
        plugin: cmake
```

And precede your `apps` entry like this:

```yaml
apps:
    app-name:
        command: snapcraft-preload <binary>
```
