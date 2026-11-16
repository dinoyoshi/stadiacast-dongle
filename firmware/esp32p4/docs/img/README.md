# Photos for the user guide

Not taken yet. The guide and the troubleshooting page have a `<!-- PHOTO: ... -->` comment where
each one goes; replace the comment with the image once it's here.

| file | shot | used in |
|---|---|---|
| `ports.jpg` | The board from above, both USB-C ports in view, with labels added: **OTG (the dongle)** and **programming (flash and log only)**. Also point out the status LED and the BOOT button. | [User guide: Hardware](../../README.md#hardware), [Troubleshooting: Which USB-C port?](../troubleshooting.md#which-usb-c-port) |
| `in-use.jpg` | The dongle plugged into a PC or a Switch by its OTG port, a Stadia Controller next to it with its light on. | the root [README](../../../../README.md), at the top |
| `setup-mode.jpg` (optional) | The controller in setup mode: light pulsing orange (a short GIF or MP4 shows the pulse best). | [First-time controller setup](../../README.md#first-time-controller-setup) |

Tips: daylight or a bright lamp, plain background, about 1600 px wide, JPEG under 300 KB each
(`magick in.jpg -resize 1600x -quality 82 ports.jpg`). Labels can be added in any image editor;
plain white boxes with black text read well on the board's black PCB.
