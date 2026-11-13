//! A transient layer surface: a dropdown menu, or one of its click-catchers
//! (the C bar's `bar/popup_surface.c`). Created when a dropdown opens and
//! dropped when it closes; see `docs/popups.md` for the rules behind them.
//!
//! Unlike the bar, a popup isn't redrawn on a schedule: its owner paints it
//! when its content changes (`paint`), and each paint is committed at once.
//!
//! **First paint:** a dropdown is painted right after it's created, but the
//! compositor hasn't configured it yet, and a surface can't be shown before
//! its first configure. So a paint before then only sets `needs_paint`, and
//! `configure` reports it back so the owner paints again (BUG.md, "A popup's
//! first paint never happened").

use smithay_client_toolkit::{
    compositor::{CompositorState, Region},
    reexports::{
        client::{
            QueueHandle,
            protocol::{wl_output::WlOutput, wl_surface::WlSurface},
        },
        protocols::wp::viewporter::client::{wp_viewport::WpViewport, wp_viewporter::WpViewporter},
    },
    shell::{
        WaylandSurface,
        wlr_layer::{Anchor, KeyboardInteractivity, Layer, LayerShell, LayerSurface},
    },
    shm::Shm,
};

use crate::wayland::{
    App,
    shm_buffers::{ShmBuffers, copy_pixels},
    viewporter::ViewporterData,
};

/// Layer-shell namespace of every popup surface (the C bar's).
const NAMESPACE: &str = "custombar-popup";

/// What the surface is for.
enum Kind {
    /// A dropdown menu of a fixed size.
    Menu {
        /// Width in px, as requested.
        width: i32,
        /// Height in px, as requested.
        height: i32,
    },
    /// A full-monitor transparent click-catcher.
    Catcher,
    /// A full-monitor surface that's drawn (the power menu), sized by the
    /// compositor. It's its own click-catcher: no separate catchers.
    Overlay,
}

/// Everything shared between [`PopupSurface::menu`] and [`PopupSurface::catcher`].
pub(crate) struct PopupEnv<'a> {
    /// The event queue new objects are attached to.
    pub(crate) qh: &'a QueueHandle<App>,
    /// Creates surfaces and regions.
    pub(crate) compositor: &'a CompositorState,
    /// Turns surfaces into layer surfaces.
    pub(crate) layer_shell: &'a LayerShell,
    /// For catchers' 1x1 stretched buffers, if available.
    pub(crate) viewporter: Option<&'a WpViewporter>,
    /// Test mode: surfaces take no pointer input (see `BarSurface::new`).
    pub(crate) test_mode: bool,
}

/// One popup or catcher surface.
pub(crate) struct PopupSurface {
    /// The layer surface.
    layer: LayerSurface,
    /// Menu or catcher.
    kind: Kind,
    /// A catcher's viewport, stretching its 1x1 buffer over the monitor.
    /// `None` for menus, and for catchers when the compositor has no
    /// viewporter (their buffer is then full-size).
    viewport: Option<WpViewport>,
    /// The two shm buffers, allocated at the first configure.
    buffers: Option<ShmBuffers>,
    /// Size as last configured, in px.
    size: (i32, i32),
    /// A paint was asked for before the first configure.
    needs_paint: bool,
}

impl PopupSurface {
    /// Creates a `width` x `height` dropdown on `output`'s overlay layer,
    /// anchored by `anchor` and offset by `margins` (top, right, bottom, left).
    /// The overlay layer is strictly above the catchers (top layer), so the
    /// menu always gets its own clicks first.
    pub(crate) fn menu(
        env: &PopupEnv<'_>,
        output: &WlOutput,
        (width, height): (i32, i32),
        margins: (i32, i32, i32, i32),
        anchor: Anchor,
    ) -> Self {
        let layer = new_layer(env, output, Layer::Overlay);
        layer.set_size(
            u32::try_from(width).unwrap_or(0),
            u32::try_from(height).unwrap_or(0),
        );
        layer.set_anchor(anchor);
        layer.set_margin(margins.0, margins.1, margins.2, margins.3);
        layer.commit();
        Self {
            layer,
            kind: Kind::Menu { width, height },
            viewport: None,
            buffers: None,
            size: (0, 0),
            needs_paint: false,
        }
    }

    /// Creates a drawn full-monitor surface on `output`'s overlay layer (the
    /// power menu). Its size comes from the compositor: the monitor minus the
    /// bars' reserved strips, so it's only known at the first configure.
    pub(crate) fn overlay(env: &PopupEnv<'_>, output: &WlOutput) -> Self {
        let layer = new_layer(env, output, Layer::Overlay);
        layer.set_size(0, 0);
        layer.set_anchor(Anchor::TOP | Anchor::BOTTOM | Anchor::LEFT | Anchor::RIGHT);
        layer.commit();
        Self {
            layer,
            kind: Kind::Overlay,
            viewport: None,
            buffers: None,
            size: (0, 0),
            needs_paint: false,
        }
    }

    /// Creates a transparent catcher covering all of `output`. Any click on it
    /// closes the dropdown it belongs to.
    pub(crate) fn catcher(env: &PopupEnv<'_>, output: &WlOutput) -> Self {
        let layer = new_layer(env, output, Layer::Top);
        let viewport = env
            .viewporter
            .map(|v| v.get_viewport(layer.wl_surface(), env.qh, ViewporterData));
        // Size 0 on both axes + all four anchors = "the whole monitor".
        layer.set_size(0, 0);
        layer.set_anchor(Anchor::TOP | Anchor::BOTTOM | Anchor::LEFT | Anchor::RIGHT);
        layer.commit();
        Self {
            layer,
            kind: Kind::Catcher,
            viewport,
            buffers: None,
            size: (0, 0),
            needs_paint: false,
        }
    }

    /// The surface, for matching pointer events and configures.
    pub(crate) fn wl_surface(&self) -> &WlSurface {
        self.layer.wl_surface()
    }

    /// A menu's size as requested, for the tests' `menu` command.
    /// For an overlay, its configured size ((0, 0) until the first configure).
    pub(crate) const fn menu_size(&self) -> Option<(i32, i32)> {
        match self.kind {
            Kind::Menu { width, height } => Some((width, height)),
            Kind::Overlay => Some(self.size),
            Kind::Catcher => None,
        }
    }

    /// The size as last configured, in px ((0, 0) before the first configure).
    pub(crate) const fn size(&self) -> (i32, i32) {
        self.size
    }

    /// Handles a configure. Catchers paint themselves (transparent) here. For
    /// a menu, returns true if its owner asked for a paint before this first
    /// configure and must now paint it; for an overlay, also whenever its
    /// size changed (its layout depends on it).
    pub(crate) fn configure(&mut self, shm: &Shm, width: u32, height: u32) -> bool {
        let (Ok(width), Ok(height)) = (i32::try_from(width), i32::try_from(height)) else {
            return false;
        };
        match (&self.kind, &self.viewport) {
            (Kind::Catcher, Some(viewport)) => {
                // The visible size comes from the viewport; the buffer stays 1x1.
                if width > 0 && height > 0 {
                    viewport.set_destination(width, height);
                }
                if self.buffers.is_none() {
                    self.allocate(shm, 1, 1);
                    self.present_transparent();
                } else {
                    self.layer.commit(); // apply the new destination
                }
                false
            }
            (Kind::Catcher, None) => {
                if self.size != (width, height) {
                    self.allocate(shm, width, height);
                    self.present_transparent();
                }
                false
            }
            (Kind::Menu { .. }, _) => {
                if self.size != (width, height) {
                    self.allocate(shm, width, height);
                }
                std::mem::take(&mut self.needs_paint)
            }
            (Kind::Overlay, _) => {
                // Its layout depends on its size, so a new size means a repaint.
                let resized = self.size != (width, height);
                if resized {
                    self.allocate(shm, width, height);
                }
                std::mem::take(&mut self.needs_paint) || resized
            }
        }
    }

    /// Paints the menu: `draw` draws onto a fresh image of the menu's size,
    /// which is copied into a free shm buffer and committed. Before the first
    /// configure this only records that a paint is wanted (see module docs).
    pub(crate) fn paint(&mut self, draw: impl FnOnce(&cairo::Context) -> Result<(), cairo::Error>) {
        let Some(buffers) = self.buffers.as_mut() else {
            self.needs_paint = true;
            return;
        };
        let (width, height) = self.size;
        // Drawn into a temporary image that's freed right after the copy, so a
        // menu's memory is just its shm buffer while it sits open.
        let image = match render_image(width, height, draw) {
            Ok(image) => image,
            Err(err) => {
                eprintln!("stickBar: drawing a popup failed: {err}");
                return;
            }
        };
        let surface = self.layer.wl_surface();
        let mut read_ok = false;
        let attached = buffers.write_and_attach(surface, |canvas| {
            let mut copied = false;
            read_ok = image
                .with_data(|pixels| copied = copy_pixels(canvas, pixels))
                .is_ok();
            read_ok && copied
        });
        if attached {
            surface.damage_buffer(0, 0, width, height);
            self.layer.commit();
        } else if !read_ok {
            eprintln!("stickBar: couldn't read a drawn popup frame");
        }
        // If both buffers were busy (rare: the compositor holds a popup's
        // buffer only until the next commit), this paint is skipped; the next
        // change paints again.
    }

    /// The surface was closed by the compositor: it can't be shown until it's
    /// recreated, so drop the buffers (a later paint just waits).
    pub(crate) fn closed(&mut self) {
        self.buffers = None;
    }

    /// (Re)allocates the shm buffers for a `width` x `height` surface.
    fn allocate(&mut self, shm: &Shm, width: i32, height: i32) {
        let stride = u32::try_from(width)
            .ok()
            .and_then(|w| cairo::Format::ARgb32.stride_for_width(w).ok());
        let buffers = stride
            .ok_or_else(|| "bad width".to_owned())
            .and_then(|stride| ShmBuffers::new(shm, width, height, stride));
        match buffers {
            Ok(buffers) => {
                self.buffers = Some(buffers);
                self.size = (width, height);
            }
            Err(err) => eprintln!("stickBar: failed to allocate a {width}x{height} popup: {err}"),
        }
    }

    /// Commits a fully transparent buffer (a catcher's only content).
    fn present_transparent(&mut self) {
        let Some(buffers) = self.buffers.as_mut() else {
            return;
        };
        let surface = self.layer.wl_surface();
        // All-zero ARGB is fully transparent.
        if buffers.write_and_attach(surface, |canvas| {
            canvas.fill(0);
            true
        }) {
            surface.damage_buffer(0, 0, self.size.0, self.size.1);
            self.layer.commit();
        }
    }
}

impl Drop for PopupSurface {
    /// Destroys the viewport before the surface goes (SCTK destroys the layer
    /// surface and `wl_surface` itself when `layer` is dropped, right after this).
    fn drop(&mut self) {
        if let Some(viewport) = &self.viewport {
            viewport.destroy();
        }
    }
}

/// A surface on `layer` of `output`, isolated from input in test mode.
fn new_layer(env: &PopupEnv<'_>, output: &WlOutput, layer: Layer) -> LayerSurface {
    let surface = env.compositor.create_surface(env.qh);
    if env.test_mode {
        // Same as the bar: an empty input region, so the user's real clicks
        // pass through a test instance's popups and catchers.
        match Region::new(env.compositor) {
            Ok(region) => surface.set_input_region(Some(region.wl_region())),
            Err(err) => eprintln!("stickBar: couldn't create an empty input region: {err}"),
        }
    }
    let layer =
        env.layer_shell
            .create_layer_surface(env.qh, surface, layer, Some(NAMESPACE), Some(output));
    layer.set_keyboard_interactivity(KeyboardInteractivity::None);
    layer
}

/// Runs `draw` on a fresh `width` x `height` image and returns it (the cairo
/// context is gone by then, so the pixels can be read).
pub(crate) fn render_image(
    width: i32,
    height: i32,
    draw: impl FnOnce(&cairo::Context) -> Result<(), cairo::Error>,
) -> Result<cairo::ImageSurface, cairo::Error> {
    let image = cairo::ImageSurface::create(cairo::Format::ARgb32, width, height)?;
    let cr = cairo::Context::new(&image)?;
    draw(&cr)?;
    drop(cr); // the context must be gone before the pixels are read
    image.flush();
    Ok(image)
}
