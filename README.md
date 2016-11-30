# Use

Add this as a part to your `snapcraft.yaml`:

```yaml
parts:
    preload:
        source: https://github.com/sergiusens/snapcraft-preload.git
        source-type: git
        plugin: cmake
```

And precede your `apps` entry like this:

```yaml
apps:
    app-name:
        command: preload <binary>
```
