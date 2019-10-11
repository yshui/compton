Compton
=======

**This is a development branch, bug to be expected**

This is forked from the original Compton because that seems to have become unmaintained.

The current battle plan of this fork is to refactor it to make the code _possible_ to maintain, so potential contributors won't be scared away when they take a look at the code.

We also try to fix bugs.

The original README can be found [here](README_orig.md)

## Change Log

See [Releases](https://github.com/yshui/compton/releases)

## Build

### Dependencies

Assuming you already have all the usual building tools installed (e.g. gcc, python, meson, ninja, etc.), you still need:

* libx11
* libx11-xcb
* libXext
* xproto
* xcb
* xcb-damage
* xcb-xfixes
* xcb-shape
* xcb-renderutil
* xcb-render
* xcb-randr
* xcb-composite
* xcb-image
* xcb-present
* xcb-xinerama
* pixman
* libdbus (optional, disable with the `-Ddbus=false` meson configure flag)
* libconfig (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libxdg-basedir (optional, disable with the `-Dconfig_file=false` meson configure flag)
* libGL (optional, disable with the `-Dopengl=false` meson configure flag)
* libpcre (optional, disable with the `-Dregex=false` meson configure flag)
* libev
* uthash

On Debian based distributions (e.g. Ubuntu), the list of needed packages are

```
libxext-dev libxcb1-dev libxcb-damage0-dev libxcb-xfixes0-dev libxcb-shape0-dev libxcb-render-util0-dev libxcb-render0-dev libxcb-randr0-dev libxcb-composite0-dev libxcb-image0-dev libxcb-present-dev libxcb-xinerama0-dev libpixman-1-dev libdbus-1-dev libconfig-dev libxdg-basedir-dev libgl1-mesa-dev  libpcre2-dev  libevdev-dev uthash-dev libev-dev
```

To build the documents, you need `asciidoc`

### To build

```bash
$ git submodule update --init --recursive
$ meson --buildtype=release . build
$ ninja -C build
```

Built binary can be found in `build/src`

If you have libraries and/or headers installed at non-default location (e.g. under `/usr/local/`), you might need to tell meson about them, since meson doesn't look for dependencies there by default.

You can do that by setting the `CPPFLAGS` and `LDFLAGS` environment variables when running `meson`. Like this:

```bash
$ LDFLAGS="-L/path/to/libraries" CPPFLAGS="-I/path/to/headers" meson --buildtype=release . build

```

As an example, on FreeBSD, you might have to run meson with:
```bash
$ LDFLAGS="-L/usr/local/include" CPPFLAGS="-I/usr/local/include" meson --buildtype=release . build
$ ninja -C build
```

### To install

``` bash
$ ninja -C build install
```

Default install prefix is `/usr/local`, you can change it with `meson configure -Dprefix=<path> build`

## How to Contribute

### Code

You can look at the [Projects](https://github.com/yshui/compton/projects) page, and see if there is anything interests you. Or you can take a look at the [Issues](https://github.com/yshui/compton/issues).

### Non-code

Even if you don't want to contribute code, you can still contribute by compiling and running this branch, and report any issue you can find.

Contributions to the documents and wiki will also be appreciated.

## Contributors

See [CONTRIBUTORS](CONTRIBUTORS)
