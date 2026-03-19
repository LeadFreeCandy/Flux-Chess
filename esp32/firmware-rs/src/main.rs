#![no_std]
#![no_main]
#![allow(unused)]

extern crate alloc;

use esp_alloc as _;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

esp_bootloader_esp_idf::esp_app_desc!();

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
mod storage;

use board::Board;
use hardware::Hardware;
use serial_server::SerialServer;
use storage::Storage;

#[esp_hal::main]
fn main() -> ! {
    esp_alloc::heap_allocator!(size: 65536);

    let peripherals = esp_hal::init(esp_hal::Config::default());

    let usb_serial = UsbSerialJtag::new(peripherals.USB_DEVICE);
    let (mut rx, mut tx) = usb_serial.split();

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

    let hw = Hardware::new(
        spi, latch, oe,
        peripherals.ADC1, peripherals.ADC2,
        peripherals.GPIO1, peripherals.GPIO2, peripherals.GPIO3,
        peripherals.GPIO4, peripherals.GPIO5, peripherals.GPIO6,
        peripherals.GPIO7, peripherals.GPIO8, peripherals.GPIO9,
        peripherals.GPIO10, peripherals.GPIO11, peripherals.GPIO12,
    );
    let mut board = Board::new(hw);
    let mut server = SerialServer::new();

    let delay = Delay::new();
    delay.delay_millis(1000);

    tx.write(b"{\"type\":\"ready\"}\n").ok();

    let mut last_watchdog = esp_hal::time::Instant::now();
    let mut last_tick = esp_hal::time::Instant::now();
    loop {
        // Handle serial commands
        let mut buf = [0u8; 64];
        let count = rx.drain_rx_fifo(&mut buf);
        for i in 0..count {
            server.feed(buf[i], &mut board, &mut tx);
        }

        // Board monitor tick (~20Hz)
        if last_tick.elapsed().as_millis() >= 50 {
            last_tick = esp_hal::time::Instant::now();
            board.tick();
        }

        // Drain events and send over serial
        for event in board.drain_events() {
            if let Ok(json) = serde_json::to_string(&event) {
                let line = alloc::format!("{{\"type\":\"event\",{}}}\n", &json[1..json.len()-1]);
                tx.write(line.as_bytes()).ok();
            }
        }

        // Watchdog
        if last_watchdog.elapsed().as_millis() >= 100 {
            last_watchdog = esp_hal::time::Instant::now();
            board.watchdog_tick();
        }

        if count == 0 {
            core::hint::spin_loop();
        }
    }
}
