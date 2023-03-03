### sbig-util

sbig-util is a set of Linux command line tools for controlling cameras
from the Santa Barbara Instrument Group.  The goal is to develop a simple
tool that can directly control a camera and filter wheel in the field
during an astrophotography session, and be part of a workflow that
includes other AIPS tools such as [ds9](http://ds9.si.edu).

### Prerequisites

The proprietary SBIG universal driver is required by this project.
If running on Ubuntu or Raspbian distros, the easiest way to install
this is to grab the `libsbigudrv2` deb package from the
[INDI](http://www.indilib.org/download.html) project.  Otherwise,
find a copy at the Diffraction Limited web site (or request it on their
SBIG discussion forum), and follow their instructions to install udev
rules and firmware.  Put the library and include file where sbig-util's
configure script can find it, e.g.
```
/usr/local/lib/libsbigudrv.so
/usr/local/include/sbigudrv.h
```

In addition, you will need the following packages (e.g. raspbian):
```
sudo apt-get install fxload
sudo apt-get install libusb-1.0-0-dev
sudo apt-get install libcfitsio-dev
```

Also, if you want to use `ds9` for FITS previewing:
```
sudo apt-get install saods9
sudo apt-get install xpa-tools
```

### Building sbig-util

To build sbig-util, run
```
./autogen.sh       # not necessary if building a release tarball
./configure
make
sudo make install  # install to /usr/local
```
If you want to cleanup the source tree after build, run
```
make maintainer-clean
```

### Configuring sbig-util

sbig-util is configurable via a config file which should be placed
in `$HOME/.sbig/config.ini`.  Here is an example:
```
[system]
device = USB1               ; USB1 thru USB8, ...
imagedir = /tmp             ; FITS files will be created here
;sbigudrv = /usr/local/lib/libsbigudrv.so

[ds9]
;xpa_nsinet = 10.10.10.253:14285 ; set for remote ds9 display

[cfw]
slot1 = Astrodon Tru-balance E-series NIR blocked R
slot2 = Astrodon Tru-balance E-series NIR blocked G
slot3 = Astrodon Tru-balance E-series NIR blocked B
slot4 = Astrodon Tru-balance E-series NIR blocked L

[config]
observer = Jim Garlick     ; Telescope operator
;filter = cfw               ; Filter selected by CFW
filter = Astrodon Tru-balance E-series NIR blocked L ; Fixed filter
telescope = Nikkor-Q       ; Telescope (CLA-7 Nikon adapter, stopped at f/4)
focal_length = 135         ; Focal length of the telescope in mm
aperture_diameter = 33     ; Aperture diameter of the telescope in mm
aperture_area = 854.86     ; Aperture area in sq-mm (correct for obstruction)

[site]
name = Carnelian Bay, CA
latitude = +39:13:36.6636   ; Latitude, degrees
longitude = +120:04:54.6924 ; Longitude, degrees W. of zero
elevation = 1928            ; Elevation, meters
```

Note: if you set `xpa_nsinet` for remote ds9 previewing, you will need
to tell ds9 to allow that and also to bind to an interface other than
localhost.  For example if the ds9 host is 10.10.10.253 and the sbig host
is 10.10.10.103, then you can launch ds9 as follows:
```
echo "DS9:ds9 10.10.10.103 +" >$HOME/acls.xpa
export XPA_HOST=10.10.10.253
ds9
```

### Running sbig-find

With your camera plugged (or on the local ethernet), `sbig-find`
should be able to probe it, e.g.
```
$ sbig find
LPT1: ST-5C 'SBIG ST-5C Camera' serial-unknown
```

### Running sbig-info

sbig-info can query info about the various parts of your system,
as well as calculate filed of view and pixel resolution based
on your camera's sensor size and the focal length you put in the
config file.

```
Usage: sbig-info driver
       sbig-info ccd [tracking|imaging]
       sbig-info cfw
       sbig-info cooler
       sbig-info fov {tracking|imaging} {lo|med|hi} [focal-length]
```
For example, with your camera connected, run:
```
sbig info ccd imaging
```
My ST-8 looks like this:
```
sbig-info: firmware-version: 2.42
sbig-info: camera-type:      ST-8
sbig-info: name:             SBIG ST-8 Dual CCD Camera
sbig-info: readout-modes:
sbig-info:  0: 1530 x 1020 2.78 e-/ADU 9.00 x 9.00 microns
sbig-info:  1:  765 x 510  2.78 e-/ADU 18.00 x 18.00 microns
sbig-info:  2:  510 x 340  2.78 e-/ADU 27.00 x 27.00 microns
sbig-info:  3: 1530 x 0    2.78 e-/ADU 9.00 x 9.00 microns
sbig-info:  4:  765 x 0    2.78 e-/ADU 18.00 x 9.00 microns
sbig-info:  5:  510 x 0    2.78 e-/ADU 27.00 x 9.00 microns
sbig-info:  6: 1530 x 1020 2.78 e-/ADU 9.00 x 9.00 microns
sbig-info:  7:  765 x 510  2.78 e-/ADU 18.00 x 18.00 microns
sbig-info:  8:  510 x 340  2.78 e-/ADU 27.00 x 27.00 microns
sbig-info:  9:  170 x 113  2.78 e-/ADU 81.00 x 81.00 microns
sbig-info: bad columns:       0
sbig-info: ABG:               no
sbig-info: serial-number:     05121350
sbig-info: ccd-type:          full frame
sbig-info: electronic-shutter:no
sbig-info: remote-guide-port: no
sbig-info: biorad-tdi-mode:   no
sbig-info: AO8-detected:      no
sbig-info: frame-buffer:      no
sbig-info: use-startexp2:     no
```

### Running sbig-cooler

sbig-cooler can enable/disable the thermoelectric cooler and
change the setpoint.
```
Usage: sbig-cooler on setpoint-degrees-C
                   off
```
For example, to set the cooler for -30C and watch it stablize:
```
sbig cooler on -30
sbig info cooler
```

### Running sbig-focus

sbig-focus is used in conjunction with ds9 for live focusing and
alignment.  It continues to run until the user aborts it with ctrl-C.

```
Usage: sbig-focus [OPTIONS]
  -t, --exposure-time SEC exposure time in seconds (default 1.0)
  -C, --ccd-chip CHIP     use imaging, tracking, or ext-tracking
  -r, --resolution RES    select hi, med, or lo resolution
  -p, --partial N         take centered partial frame (0 < N <= 1.0)
```

To focus/align your camera using full frame, 3X3 binned (lo resolution),
and 1 second exposures, first start ds9, then run:
```
sbig focus
```

### Running sbig-cfw

sbig-cfw controls an SBIG filter wheel.  Slots are numbered 1-N.
```
Usage: sbig-cfw query
       sbig-cfw goto N
```

### Running sbig-snap

sbig-snap is used for taking images, which are written as FITS files
to the configured image directory.
```
Usage: sbig-snap [OPTIONS]
  -t, --exposure-time SEC    exposure time in seconds (default 1.0)
  -d, --image-directory DIR  where to put images (default /tmp)
  -C, --ccd-chip CHIP        use imaging, tracking, or ext-tracking
  -r, --resolution RES       select hi, med, or lo resolution
  -n, --count N              take N exposures
  -D, --time-delta N         increase exposure time by N on each exposure
  -m, --message string       add COMMENT to FITS file
  -O, --object NAME          name of object being observed (e.g. M33)
  -f, --force                press on even if FITS header will be incomplete
  -p, --partial N            take centered partial frame (0 < N <= 1.0)
  -P, --preview              preview image using ds9
  -T, --image-type TYPE      take df, lf, or auto (default auto)
  -c, --no-cooler            allow TE to be disabled/unstable
```

To take a full frame, high resolution, auto-dark-subtracted, 30s
exposure of M31:
```
sbig snap --object M31 -t 30
```

### FITS headers

sbig-util writes FITS files using SBIG FITS header extensions, described in
[SBFITSEXT Version 1.0: A Set of FITS Standard Extensions for Amateur
Astronomical Processing Software Packages March 19, 2003](http://diffractionlimited.com/wp-content/uploads/2016/11/sbfitsext_1r0.pdf)

Here is an example header:
```
SIMPLE  =                    T / file does conform to FITS standard
BITPIX  =                   16 / number of bits per data pixel
NAXIS   =                    2 / number of data axes
NAXIS1  =                 1530 / length of data axis 1
NAXIS2  =                 1020 / length of data axis 2
EXTEND  =                    T / FITS dataset may contain extensions
COMMENT   FITS (Flexible Image Transport System) format is defined in 'Astronomy
COMMENT   and Astrophysics', volume 376, page 359; bibcode: 2001A&A...376..359H
BZERO   =                32768 / offset data range to that of unsigned short
BSCALE  =                    1 / default scaling factor
COMMENT = 'SBIG FITS header format per:'
COMMENT = ' http://www.sbig.com/pdffiles/SBFITSEXT_1r0.pdf'
SBSTDVER= 'SBFITSEXT Version 1.0' / SBIG FITS extensions ver
DATE    = '2014-10-31T06:11:26' / GMT date when this file created
DATE-OBS= '2014-10-31T06:11:33' / GMT start of exposure
EXPTIME =                   1. / Exposure in seconds
CCD-TEMP=     29.3146109989742 / CCD temp in degress C
SET-TEMP=    0.189018727865133 / Setpoint for CCD temp in degress C
SWCREATE= 'sbig-util 0.1.0'    / Software that created this image
SWMODIFY= 'sbig-util 0.1.0'    / Software that modified this image
HISTORY = 'Dark Subtraction'   / How modified
SITENAME= 'Carnelian Bay, CA'  / Site name
SITEELEV=                1928. / Site elevation in meters
SITELAT = '+39:13:36.6636'     / Site latitude in degrees
SITELONG= '+120:04:54.6924'    / Site longitude in degrees west of zero
OBJECT  = 'M32     '           / Name of object imaged
TELESCOP= 'Nikkor-Q 135mm'     / Telescope model
FILTER  = 'Astrodon Tru-balance E-series NIR blocked L' / Optical filter name
OBSERVER= 'Jim Garlick'        / Telescope operator
INSTRUME= 'SBIG ST-8 Dual CCD Camera' / Camera Model
XBINNING=                    1 / Horizontal binning factor
YBINNING=                    1 / Vertical binning factor
XPIXSZ  =                   9. / Pixel width in microns
YPIXSZ  =                   9. / Pixel height in microns
EGAIN   =                 2.78 / Electrons per ADU
XORGSUBF=                    0 / Subframe origin x_pos
YORGSUBF=                    0 / Subframe origin y_pos
RESMODE =                    0 / Resolution mode
SNAPSHOT=                    1 / Number images coadded
FOCALLEN=                 135. / Focal length in mm
APTDIA  =                  33. / Aperture diameter in mm
APTAREA =               854.86 / Aperture area in sq-mm
CBLACK  =                  138 / Black ADU for display
CWHITE  =                   74 / White ADU for display
PEDESTAL=                 -100 / Add to ADU for 0-base
DATAMAX =                40000 / Saturation level
END
```

### Parallel Port Cameras

See my other projects to revive support for the older parallel port
based cameras:

http://github.com/garlick/sbig-parport

http://github.com/garlick/pi-parport
