extern crate alloc;
use alloc::string::String;
use alloc::format;
use alloc::vec::Vec;

use esp_hal::usb_serial_jtag::UsbSerialJtagTx;
use esp_hal::Blocking;

use crate::api::*;
use crate::board::Board;

// ── JSON parse helpers ────────────────────────────────────────

pub fn json_get(json: &str, key: &str) -> String {
    let search = format!("\"{}\"", key);
    let Some(idx) = json.find(&search) else { return String::new() };
    let after_key = &json[idx + search.len()..];
    let Some(colon) = after_key.find(':') else { return String::new() };
    let val_str = after_key[colon + 1..].trim_start();

    if val_str.starts_with('"') {
        let inner = &val_str[1..];
        let Some(end) = inner.find('"') else { return String::new() };
        inner[..end].into()
    } else {
        let end = val_str.find(&[',', '}'][..]).unwrap_or(val_str.len());
        val_str[..end].trim().into()
    }
}

fn json_get_obj(json: &str, key: &str) -> String {
    let search = format!("\"{}\"", key);
    let Some(idx) = json.find(&search) else { return String::new() };
    let after_key = &json[idx + search.len()..];
    let Some(brace) = after_key.find('{') else { return String::new() };
    let from_brace = &after_key[brace..];

    let mut depth = 0;
    for (i, c) in from_brace.chars().enumerate() {
        match c {
            '{' => depth += 1,
            '}' => {
                depth -= 1;
                if depth == 0 {
                    return from_brace[..=i].into();
                }
            }
            _ => {}
        }
    }
    String::new()
}

// ── Command handler type ──────────────────────────────────────

struct Command {
    name: &'static str,
    handler: fn(&mut Board<'_>, &str) -> String,
}

// ── Built-in handlers ─────────────────────────────────────────

fn handle_pulse_coil(board: &mut Board<'_>, params: &str) -> String {
    let x: u8 = json_get(params, "x").parse().unwrap_or(0);
    let y: u8 = json_get(params, "y").parse().unwrap_or(0);
    let dur: u16 = json_get(params, "duration_ms").parse().unwrap_or(0);
    let res = board.pulse_coil(x, y, dur);
    serde_json::to_string(&res).unwrap_or_else(|_| "{}".into())
}

fn handle_get_board_state(board: &mut Board<'_>, _params: &str) -> String {
    let res = board.get_board_state();
    serde_json::to_string(&res).unwrap_or_else(|_| "{}".into())
}

fn handle_set_rgb(board: &mut Board<'_>, params: &str) -> String {
    let r: u8 = json_get(params, "r").parse().unwrap_or(0);
    let g: u8 = json_get(params, "g").parse().unwrap_or(0);
    let b: u8 = json_get(params, "b").parse().unwrap_or(0);
    let res = board.set_rgb(r, g, b);
    serde_json::to_string(&res).unwrap_or_else(|_| "{}".into())
}

fn handle_shutdown(board: &mut Board<'_>, _params: &str) -> String {
    serde_json::to_string(&ShutdownResponse {}).unwrap_or_else(|_| "{}".into())
}

// ── Serial Server ─────────────────────────────────────────────

pub struct SerialServer {
    line_buf: String,
    commands: Vec<Command>,
}

impl SerialServer {
    pub fn new() -> Self {
        let mut s = Self {
            line_buf: String::new(),
            commands: Vec::new(),
        };
        s.on("pulse_coil", handle_pulse_coil);
        s.on("get_board_state", handle_get_board_state);
        s.on("set_rgb", handle_set_rgb);
        s.on("shutdown", handle_shutdown);
        s
    }

    pub fn on(&mut self, name: &'static str, handler: fn(&mut Board<'_>, &str) -> String) {
        self.commands.push(Command { name, handler });
    }

    pub fn feed(&mut self, byte: u8, board: &mut Board<'_>, tx: &mut UsbSerialJtagTx<'_, Blocking>) -> bool {
        if byte == b'\n' {
            let line: String = self.line_buf.trim().into();
            self.line_buf.clear();
            if !line.is_empty() {
                self.handle_command(&line, board, tx);
                return true;
            }
        } else {
            self.line_buf.push(byte as char);
        }
        false
    }

    fn handle_command(&self, line: &str, board: &mut Board<'_>, tx: &mut UsbSerialJtagTx<'_, Blocking>) {
        let method = json_get(line, "method");
        if method.is_empty() { return; }

        let params = json_get_obj(line, "params");

        for cmd in &self.commands {
            if method == cmd.name {
                let result = (cmd.handler)(board, &params);
                let response = format!("{{\"method\":\"{}\",\"result\":{}}}\n", cmd.name, result);
                tx.write(response.as_bytes()).ok();

                if method == "shutdown" {
                    board.shutdown(); // never returns
                }
                return;
            }
        }

        let err = format!("{{\"method\":\"{}\",\"error\":\"unknown command\"}}\n", method);
        tx.write(err.as_bytes()).ok();
    }
}
