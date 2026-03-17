extern crate alloc;
use alloc::string::String;
use alloc::format;
use alloc::boxed::Box;
use alloc::vec::Vec;

use crate::api::*;
use crate::board::Board;

// ── Command Registry ──────────────────────────────────────────

type CommandHandler = Box<dyn Fn(&mut Board<'_>, &str) -> String>;

struct Command {
    name: &'static str,
    handler: fn(&mut Board<'_>, &str) -> String,
}

// ── JSON parse helpers ────────────────────────────────────────

pub fn json_get(json: &str, key: &str) -> String {
    let search = format!("\"{}\"", key);
    let Some(idx) = json.find(&search) else { return String::new() };
    let after_key = &json[idx + search.len()..];
    let Some(colon) = after_key.find(':') else { return String::new() };
    let val_str = after_key[colon + 1..].trim_start();

    if val_str.starts_with('"') {
        // String value
        let inner = &val_str[1..];
        let Some(end) = inner.find('"') else { return String::new() };
        inner[..end].into()
    } else {
        // Number/bool value
        let end = val_str.find([',', '}'].as_ref()).unwrap_or(val_str.len());
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

// ── Built-in command handlers ─────────────────────────────────

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
    let res = ShutdownResponse {};
    let json = serde_json::to_string(&res).unwrap_or_else(|_| "{}".into());
    // Return the response — the caller sends it then calls board.shutdown()
    json
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

    /// Feed a byte from serial. Returns true if a command was processed.
    pub fn feed(&mut self, byte: u8, board: &mut Board<'_>) -> bool {
        if byte == b'\n' {
            let line = core::mem::take(&mut self.line_buf);
            let trimmed = line.trim();
            if !trimmed.is_empty() {
                self.handle_command(trimmed, board);
                return true;
            }
        } else {
            self.line_buf.push(byte as char);
        }
        false
    }

    fn handle_command(&self, line: &str, board: &mut Board<'_>) {
        let method = json_get(line, "method");
        if method.is_empty() { return; }

        let params = json_get_obj(line, "params");

        for cmd in &self.commands {
            if method == cmd.name {
                let result = (cmd.handler)(board, &params);
                esp_println::print!("{{\"method\":\"{}\",\"result\":{}}}\n", cmd.name, result);

                if method == "shutdown" {
                    board.shutdown(); // never returns
                }
                return;
            }
        }

        esp_println::print!("{{\"method\":\"{}\",\"error\":\"unknown command\"}}\n", method);
    }
}
