#![no_std]
#![no_main]
#![allow(unused)]

extern crate alloc;

use esp_alloc as _;
use esp_backtrace as _;
use esp_hal::gpio::{Level, Output, OutputConfig};
use esp_hal::spi::master::{Config as SpiConfig, Spi};
use esp_hal::spi::Mode as SpiMode;
use esp_hal::time::Rate;
use esp_hal::usb_serial_jtag::UsbSerialJtag;
use esp_hal::delay::Delay;

mod api;
mod board;
mod hardware;
mod pins;
mod serial_server;

use board::Board;
use hardware::Hardware;
use serial_server::SerialServer;

#[esp_hal::main]
fn main() -> ! {
    esp_alloc::heap_allocator!(size: 65536);

    let peripherals = esp_hal::init(esp_hal::Config::default());

    // USB Serial/JTAG — split into rx and tx
    let usb_serial = UsbSerialJtag::new(peripherals.USB_DEVICE);
    let (mut rx, mut tx) = usb_serial.split();

    // SPI for shift registers
    let spi = Spi::new(
        peripherals.SPI2,
        SpiConfig::default()
            .with_frequency(Rate::from_mhz(4))
            .with_mode(SpiMode::_0),
    )
    .unwrap()
    .with_sck(peripherals.GPIO39)
    .with_mosi(peripherals.GPIO40);

    let latch = Output::new(peripherals.GPIO42, Level::Low, OutputConfig::default());
    let oe = Output::new(peripherals.GPIO48, Level::High, OutputConfig::default());

    let hw = Hardware::new(spi, latch, oe);
    let mut board = Board::new(hw);
    let mut server = SerialServer::new();

    let delay = Delay::new();
    delay.delay_millis(1000);

    tx.write(b"{\"type\":\"ready\"}\n").ok();

    let mut last_watchdog = esp_hal::time::Instant::now();
    loop {
        // Drain available serial bytes
        let mut buf = [0u8; 64];
        let count = rx.drain_rx_fifo(&mut buf);
        for i in 0..count {
            server.feed(buf[i], &mut board, &mut tx);
        }

        // Watchdog tick every 100ms
        if last_watchdog.elapsed().as_millis() >= 100 {
            last_watchdog = esp_hal::time::Instant::now();
            board.watchdog_tick();
        }

        // Hint CPU we're polling — reduces power in tight loops
        if count == 0 {
            core::hint::spin_loop();
        }
    }
}
