extern crate alloc;
use alloc::vec::Vec;

use esp_nvs::{Nvs, Key};
use esp_storage::FlashStorage;
use crate::api::CalibrationResult;

// NVS partition: matches default espflash partition table
const NVS_OFFSET: usize = 0x9000;
const NVS_SIZE: usize = 0x6000;

pub struct Storage {
    nvs: Nvs<FlashStorage<'static>>,
}

fn key(s: &str) -> Key {
    Key::from_str(s)
}

impl Storage {
    pub fn new(flash: esp_hal::peripherals::FLASH<'static>) -> Option<Self> {
        let storage = FlashStorage::new(flash);
        let nvs = Nvs::new(NVS_OFFSET, NVS_SIZE, storage).ok()?;
        log::info!("NVS initialized");
        Some(Self { nvs })
    }

    pub fn save_calibration(&mut self, cal: &CalibrationResult) -> bool {
        let Ok(json) = serde_json::to_vec(cal) else { return false };
        match self.nvs.set::<&[u8]>(&key("fluxchess"), &key("cal"), &json) {
            Ok(_) => {
                log::info!("Calibration saved ({} bytes)", json.len());
                true
            }
            Err(e) => {
                log::error!("Failed to save calibration: {:?}", e);
                false
            }
        }
    }

    pub fn load_calibration(&mut self) -> Option<CalibrationResult> {
        let data: Vec<u8> = self.nvs.get(&key("fluxchess"), &key("cal")).ok()?;
        let cal = serde_json::from_slice(&data).ok()?;
        log::info!("Calibration loaded from NVS");
        Some(cal)
    }
}
