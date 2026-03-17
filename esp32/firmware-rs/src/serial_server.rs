extern crate alloc;
use alloc::string::String;
use alloc::format;

use esp_hal::usb_serial_jtag::UsbSerialJtagTx;
use esp_hal::Blocking;
use serde::de::DeserializeOwned;
use serde::Serialize;

use crate::api::*;
use crate::board::Board;

// ── Command macro ─────────────────────────────────────────────
//
// Generates the command table from one-liner definitions:
//
//   commands! {
//       "pulse_coil" => PulseCoilRequest => |b, r| b.pulse_coil(r.x, r.y, r.duration_ms),
//       "get_board_state" => () => |b, _| b.get_board_state(),
//   }
//

macro_rules! commands {
    ($($name:literal => () => |$b:ident, $_:ident| $body:expr),* $(,)?) => {
        &[$(
            Command {
                name: $name,
                handler: |$b: &mut Board<'_>, _params: &str| -> String {
                    let res = $body;
                    serde_json::to_string(&res).unwrap_or_else(|_| "{}".into())
                },
            },
        )*]
    };
    // Can't mix () and typed arms in one macro_rules — use a combined approach
}

// Two-phase: typed requests get deserialized, () requests skip parsing
struct Command {
    name: &'static str,
    handler: fn(&mut Board<'_>, &str) -> String,
}

fn make_handler<Req, Res, F>(f: F) -> fn(&mut Board<'_>, &str) -> String
where
    Req: DeserializeOwned,
    Res: Serialize,
    F: Fn(&mut Board<'_>, Req) -> Res + 'static,
{
    // Can't use closures as fn pointers with captures.
    // Use a different approach below.
    unreachable!()
}

// Since Rust fn pointers can't capture, we use a macro to inline everything:

macro_rules! typed_command {
    ($name:literal, $req:ty, |$b:ident, $r:ident| $body:expr) => {
        Command {
            name: $name,
            handler: |$b: &mut Board<'_>, params: &str| -> String {
                let $r: $req = match serde_json::from_str(params) {
                    Ok(r) => r,
                    Err(_) => return format!("{{\"error\":\"invalid params for {}\"}}",  $name),
                };
                let res = $body;
                serde_json::to_string(&res).unwrap_or_else(|_| "{}".into())
            },
        }
    };
}

macro_rules! void_command {
    ($name:literal, |$b:ident| $body:expr) => {
        Command {
            name: $name,
            handler: |$b: &mut Board<'_>, _params: &str| -> String {
                let res = $body;
                serde_json::to_string(&res).unwrap_or_else(|_| "{}".into())
            },
        }
    };
}

// ── Command table ─────────────────────────────────────────────

fn build_commands() -> &'static [Command] {
    &[
        typed_command!("pulse_coil", PulseCoilRequest,
            |board, req| board.pulse_coil(req.x, req.y, req.duration_ms)),
        void_command!("get_board_state",
            |board| board.get_board_state()),
        typed_command!("set_rgb", SetRGBRequest,
            |board, req| board.set_rgb(req.r, req.g, req.b)),
        void_command!("shutdown",
            |board| ShutdownResponse {}),
    ]
}

// ── JSON envelope parsing ─────────────────────────────────────
// We only need to extract "method" and "params" from the outer envelope.
// The params object is passed as raw JSON to serde for deserialization.

fn extract_method(json: &str) -> Option<&str> {
    let needle = "\"method\":\"";
    let start = json.find(needle)? + needle.len();
    let end = json[start..].find('"')? + start;
    Some(&json[start..end])
}

fn extract_params(json: &str) -> &str {
    let needle = "\"params\":";
    match json.find(needle) {
        Some(idx) => {
            let rest = &json[idx + needle.len()..];
            let rest = rest.trim_start();
            if rest.starts_with('{') {
                let mut depth = 0;
                for (i, c) in rest.chars().enumerate() {
                    match c {
                        '{' => depth += 1,
                        '}' => {
                            depth -= 1;
                            if depth == 0 { return &rest[..=i]; }
                        }
                        _ => {}
                    }
                }
            }
            "{}"
        }
        None => "{}",
    }
}

// ── Serial Server ─────────────────────────────────────────────

pub struct SerialServer {
    line_buf: String,
    commands: &'static [Command],
}

impl SerialServer {
    pub fn new() -> Self {
        Self {
            line_buf: String::new(),
            commands: build_commands(),
        }
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
        let Some(method) = extract_method(line) else { return };
        let params = extract_params(line);

        for cmd in self.commands {
            if method == cmd.name {
                let result = (cmd.handler)(board, params);
                let response = format!("{{\"method\":\"{}\",\"result\":{}}}\n", cmd.name, result);
                tx.write(response.as_bytes()).ok();

                if method == "shutdown" {
                    board.shutdown();
                }
                return;
            }
        }

        let err = format!("{{\"method\":\"{}\",\"error\":\"unknown command\"}}\n", method);
        tx.write(err.as_bytes()).ok();
    }
}
