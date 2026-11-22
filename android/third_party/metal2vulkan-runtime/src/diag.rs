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

/// `REIMS_PASS_LOG=1`: the passes that render into a 1920x1080 color attachment, which on this guest
/// is the display's own surface, and every fragment texture and draw inside them. A composite that
/// comes out wrong has to be one of these: either it sampled the wrong texture, or it loaded or
/// stored the attachment in a way the pass did not mean.
pub(crate) fn passes() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("REIMS_PASS_LOG").is_some_and(|v| v != "0"))
}

std::thread_local! {
    /// Whether the encoder this thread is calling is one of the display's passes.
    static IN_DISPLAY_PASS: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    /// The pass's number, so a pass's lines can be read together.
    static PASS_NUMBER: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

/// Notes `descriptor` as the pass this thread's encoder runs: one line where it renders into a
/// 1920x1080 color attachment, and every fragment texture bound afterwards is logged with it.
pub(crate) fn begin_pass(descriptor: &crate::render_pass::RenderPassDescriptor) {
    if !passes() {
        return;
    }
    let display = descriptor.color_attachments[0]
        .texture
        .as_ref()
        .is_some_and(|t| t.width() == 1920 && t.height() == 1080);
    if !display {
        return;
    }
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    PASS_NUMBER.with(|c| c.set(n));
    IN_DISPLAY_PASS.with(|c| c.set(true));
    for (i, color) in descriptor.color_attachments.iter().enumerate() {
        if let Some(t) = &color.texture {
            eprintln!(
                "pass #{n} color{i}: {:?} {}x{} load {:?} store {:?}",
                t.pixel_format(),
                t.width(),
                t.height(),
                color.load_action,
                color.store_action
            );
        }
    }
}

/// The pass this thread's encoder runs has ended.
pub(crate) fn end_pass() {
    if passes() {
        IN_DISPLAY_PASS.with(|c| c.set(false));
    }
}

/// A fragment texture bound inside the display's pass, and the slot it takes.
pub(crate) fn bound_fragment_texture(index: u32, texture: Option<&crate::texture::Texture>) {
    if !passes() || !IN_DISPLAY_PASS.with(std::cell::Cell::get) {
        return;
    }
    let n = PASS_NUMBER.with(std::cell::Cell::get);
    match texture {
        Some(t) => eprintln!(
            "pass #{n} frag[{index}]: {:?} {}x{}",
            t.pixel_format(),
            t.width(),
            t.height()
        ),
        None => eprintln!("pass #{n} frag[{index}]: nil"),
    }
}

/// A draw inside the display's pass: what it draws and where.
pub(crate) fn drew(n: &str, vertex_count: u32, instance_count: u32) {
    if !passes() || !IN_DISPLAY_PASS.with(std::cell::Cell::get) {
        return;
    }
    let pass = PASS_NUMBER.with(std::cell::Cell::get);
    eprintln!("pass #{pass} draw {n}: vertices {vertex_count} instances {instance_count}");
}

/// Every fragment texture of `textures` bound inside the display's pass, from `first` on.
pub(crate) fn bound_fragment_textures(first: u32, textures: &[Option<&crate::texture::Texture>]) {
    if !passes() || !IN_DISPLAY_PASS.with(std::cell::Cell::get) {
        return;
    }
    for (at, texture) in textures.iter().enumerate() {
        bound_fragment_texture(first + at as u32, *texture);
    }
}

/// The viewport the display's pass draws through: the dirty rectangle a partial update covers.
pub(crate) fn viewport(v: &crate::render_values::Viewport) {
    if !passes() || !IN_DISPLAY_PASS.with(std::cell::Cell::get) {
        return;
    }
    let pass = PASS_NUMBER.with(std::cell::Cell::get);
    eprintln!(
        "pass #{pass} viewport: x {} y {} w {} h {}",
        v.origin_x, v.origin_y, v.width, v.height
    );
}

/// The scissor rectangle the display's pass draws through, the other half of a dirty region.
pub(crate) fn scissor(r: &crate::render_values::ScissorRect) {
    if !passes() || !IN_DISPLAY_PASS.with(std::cell::Cell::get) {
        return;
    }
    let pass = PASS_NUMBER.with(std::cell::Cell::get);
    eprintln!("pass #{pass} scissor: x {} y {} w {} h {}", r.x, r.y, r.width, r.height);
}
