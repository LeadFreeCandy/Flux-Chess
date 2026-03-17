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
    // Init heap allocator (64KB)
    esp_alloc::heap_allocator!(size: 65536);

    // Init peripherals
    let peripherals = esp_hal::init(esp_hal::Config::default());

    // Init logging
    esp_println::logger::init_logger_from_env();

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

    // Build the stack
    let hw = Hardware::new(spi, latch, oe);
    let mut board = Board::new(hw);
    let mut server = SerialServer::new();

    esp_println::print!("{{\"type\":\"ready\"}}\n");

    // Main loop: read serial bytes, feed to server
    let delay = esp_hal::delay::Delay::new();
    loop {
        // TODO: read from USB Serial/JTAG interface
        // For now this is a placeholder — we need to set up the USB JTAG serial
        // peripheral to read incoming bytes

        // Watchdog tick
        board.watchdog_tick();

        delay.delay_millis(100);
    }
}
