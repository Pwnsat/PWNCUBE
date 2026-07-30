#!/usr/bin/env python3
"""Direct PWNCUBE console over libusb (bulk EP), no system driver needed.
PWNCUBE's USB gadget is a vendor-specific interface (class 0xff) with two
bulk endpoints -- not CDC-ACM, so macOS never assigns it a /dev/cu.*
automatically. This script talks the bulk endpoints directly.

Usage:
  python3 pwncube_console.py "uname -a"       # one command, prints the reply
  python3 pwncube_console.py --interactive    # full-duplex, screen-like session:
                                               # a background thread prints
                                               # everything the board sends
                                               # (banners, the ~10s idle menu
                                               # reprint, replies) in real
                                               # time, while the main thread
                                               # lets you type at any moment --
                                               # no need to wait for the
                                               # previous reply to "finish".
"""
import sys
import threading
import time
import usb.core
import usb.util

VID, PID = 0x2207, 0x0011
EP_OUT, EP_IN = 0x01, 0x81


def get_device():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.exit("PWNCUBE not found (VID:PID 2207:0011). Is it connected and booted?")
    if dev.is_kernel_driver_active(0):
        dev.detach_kernel_driver(0)
    dev.set_configuration()
    usb.util.claim_interface(dev, 0)
    return dev


def drain(dev, timeout_ms=300):
    """Reads everything pending without blocking for long."""
    out = b""
    while True:
        try:
            data = dev.read(EP_IN, 512, timeout=timeout_ms)
            out += bytes(data)
        except usb.core.USBError:
            break
    return out


def send_cmd(dev, cmd, settle=1.0):
    drain(dev, 200)  # clear any pending garbage
    dev.write(EP_OUT, (cmd + "\n").encode())
    time.sleep(settle)
    return drain(dev, 500)


def _reader_thread(dev, stop_event):
    """Runs in the background for the whole interactive session: prints
    everything that arrives on EP_IN as soon as it arrives, without
    waiting for the main thread to write anything -- so banners, the
    ~10s idle menu reprint, and replies to whatever gets typed all show
    up in real time (real full-duplex, not request/response)."""
    while not stop_event.is_set():
        try:
            data = dev.read(EP_IN, 512, timeout=300)
            sys.stdout.write(bytes(data).decode(errors="replace"))
            sys.stdout.flush()
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError:
            break


def main():
    dev = get_device()
    if "--interactive" in sys.argv:
        print("PWNCUBE interactive console -- full-duplex (Ctrl+D to quit "
              "this wrapper). Whatever the board sends prints on its own, "
              "at any time; type and press Enter to send a command "
              "whenever you want. Typing 'exit' is sent to the board like "
              "anything else (e.g. to leave its Debug Shell) -- it does "
              "NOT quit this wrapper, only Ctrl+D does.\n")
        stop_event = threading.Event()
        reader = threading.Thread(target=_reader_thread, args=(dev, stop_event), daemon=True)
        reader.start()
        dev.write(EP_OUT, b"\n")  # force an initial reprint (menu or prompt)
        try:
            while True:
                line = input()
                dev.write(EP_OUT, (line + "\n").encode())
        except EOFError:
            pass
        finally:
            stop_event.set()
            time.sleep(0.4)  # let the reader flush anything still pending
    else:
        cmd = " ".join(a for a in sys.argv[1:] if a != "--interactive")
        if not cmd:
            sys.exit(__doc__)
        out = send_cmd(dev, cmd)
        sys.stdout.write(out.decode(errors="replace"))


if __name__ == "__main__":
    main()
