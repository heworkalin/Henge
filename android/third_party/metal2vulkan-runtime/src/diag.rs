//! TEMP DIAG (Android bring-up): the probes that found the GPU path's failures, printed only
//! where `REIMS_DIAG` is set. They are off by default: a run prints nothing of them, and the
//! engine's own log stays as it was. Remove them, with the calls that read `diag!`, once the
//! remaining bring-up work (the window's path, the dma-buf route) is done.

/// Whether the probes print: `REIMS_DIAG` set to anything but `0`, read once.
pub(crate) fn on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("REIMS_DIAG").is_some_and(|v| v != "0"))
}

/// `eprintln!`, where [`on`].
macro_rules! diag {
    ($($arg:tt)*) => {
        if $crate::diag::on() {
            eprintln!($($arg)*);
        }
    };
}
pub(crate) use diag;
