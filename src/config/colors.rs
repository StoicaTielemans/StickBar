//! The Catppuccin Mocha palette, plus the `Color` type every drawing call uses.

/// One RGBA colour with each channel in 0.0..=1.0, the range cairo expects.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Color {
    /// Red channel.
    pub(crate) r: f64,
    /// Green channel.
    pub(crate) g: f64,
    /// Blue channel.
    pub(crate) b: f64,
    /// Alpha (opacity) channel; 1.0 is fully opaque.
    pub(crate) a: f64,
}

impl Color {
    /// Fully transparent: "no fill" or "no border" for `draw_rect`.
    pub(crate) const TRANSPARENT: Self = Self {
        r: 0.0,
        g: 0.0,
        b: 0.0,
        a: 0.0,
    };

    /// An opaque colour from 0-255 channel values, so the palette below can be
    /// written as the same hex triples as the reference `config.js`.
    // `as` is banned crate-wide, but `f64::from(u8)` (the lossless conversion the
    // lint wants instead) can't be called inside a `const fn` on stable Rust. A
    // u8 -> f64 cast can never lose information, so this one is safe.
    #[expect(
        clippy::as_conversions,
        reason = "u8 -> f64 is lossless, and f64::from isn't usable in a const fn"
    )]
    const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Self {
            r: r as f64 / 255.0,
            g: g as f64 / 255.0,
            b: b as f64 / 255.0,
            a: 1.0,
        }
    }

    /// The same colour with its opacity scaled by `factor` (e.g. 0.4 for the
    /// calendar's past days).
    pub(crate) const fn with_alpha(self, factor: f64) -> Self {
        Self {
            a: self.a * factor,
            ..self
        }
    }

    /// The colour `t` of the way from `self` to `other` (0.0 = self, 1.0 = other),
    /// every channel including alpha.
    pub(crate) fn mix(self, other: Self, t: f64) -> Self {
        Self {
            r: self.r + (other.r - self.r) * t,
            g: self.g + (other.g - self.g) * t,
            b: self.b + (other.b - self.b) * t,
            a: self.a + (other.a - self.a) * t,
        }
    }

    /// Makes this colour cairo's current source, for the next `fill`/`stroke`/`paint`.
    pub(crate) fn set_source(self, cr: &cairo::Context) {
        cr.set_source_rgba(self.r, self.g, self.b, self.a);
    }
}

/// A three-stop gradient (`stops[0]` at t = 0, `stops[1]` at 0.5, `stops[2]`
/// at 1): the graphs' and meters' low → mid → high colouring.
pub(crate) fn gradient3(stops: &[Color; 3], t: f64) -> Color {
    let [low, mid, high] = *stops;
    if t < 0.5 {
        low.mix(mid, t * 2.0)
    } else {
        mid.mix(high, (t - 0.5) * 2.0)
    }
}

/// Every named colour of Catppuccin Mocha. The whole palette is ported at once
/// (it's one unit in the reference project), even though modules only start
/// using most of it from M3 on.
// `expect`, not `allow`: once every colour is used by some module, this
// expectation stops being met, the build fails, and the attribute gets removed.
#[expect(
    dead_code,
    reason = "full palette ported up front; modules pick colours as they land"
)]
#[derive(Debug)]
pub(crate) struct Palette {
    /// `#f5e0dc`
    pub(crate) rosewater: Color,
    /// `#f2cdcd`
    pub(crate) flamingo: Color,
    /// `#f5c2e7`
    pub(crate) pink: Color,
    /// `#cba6f7`
    pub(crate) mauve: Color,
    /// `#f38ba8`
    pub(crate) red: Color,
    /// `#eba0ac`
    pub(crate) maroon: Color,
    /// `#fab387`
    pub(crate) peach: Color,
    /// `#f9e2af`
    pub(crate) yellow: Color,
    /// `#a6e3a1`
    pub(crate) green: Color,
    /// `#94e2d5`
    pub(crate) teal: Color,
    /// `#89dceb`
    pub(crate) sky: Color,
    /// `#74c7ec`
    pub(crate) sapphire: Color,
    /// `#89b4fa`
    pub(crate) blue: Color,
    /// `#b4befe`
    pub(crate) lavender: Color,
    /// `#cdd6f4`, the default text colour.
    pub(crate) text: Color,
    /// `#bac2de`
    pub(crate) subtext1: Color,
    /// `#a6adc8`
    pub(crate) subtext0: Color,
    /// `#9399b2`
    pub(crate) overlay2: Color,
    /// `#7f849c`
    pub(crate) overlay1: Color,
    /// `#6c7086`
    pub(crate) overlay0: Color,
    /// `#585b70`
    pub(crate) surface2: Color,
    /// `#45475a`
    pub(crate) surface1: Color,
    /// `#313244`
    pub(crate) surface0: Color,
    /// `#1e1e2e`
    pub(crate) base: Color,
    /// `#181825`, the bar's background and every module box's fill.
    pub(crate) mantle: Color,
    /// `#11111b`
    pub(crate) crust: Color,
}

/// The palette itself, the same values as the C bar's `COLORS`.
pub(crate) const COLORS: Palette = Palette {
    rosewater: Color::rgb(0xf5, 0xe0, 0xdc),
    flamingo: Color::rgb(0xf2, 0xcd, 0xcd),
    pink: Color::rgb(0xf5, 0xc2, 0xe7),
    mauve: Color::rgb(0xcb, 0xa6, 0xf7),
    red: Color::rgb(0xf3, 0x8b, 0xa8),
    maroon: Color::rgb(0xeb, 0xa0, 0xac),
    peach: Color::rgb(0xfa, 0xb3, 0x87),
    yellow: Color::rgb(0xf9, 0xe2, 0xaf),
    green: Color::rgb(0xa6, 0xe3, 0xa1),
    teal: Color::rgb(0x94, 0xe2, 0xd5),
    sky: Color::rgb(0x89, 0xdc, 0xeb),
    sapphire: Color::rgb(0x74, 0xc7, 0xec),
    blue: Color::rgb(0x89, 0xb4, 0xfa),
    lavender: Color::rgb(0xb4, 0xbe, 0xfe),
    text: Color::rgb(0xcd, 0xd6, 0xf4),
    subtext1: Color::rgb(0xba, 0xc2, 0xde),
    subtext0: Color::rgb(0xa6, 0xad, 0xc8),
    overlay2: Color::rgb(0x93, 0x99, 0xb2),
    overlay1: Color::rgb(0x7f, 0x84, 0x9c),
    overlay0: Color::rgb(0x6c, 0x70, 0x86),
    surface2: Color::rgb(0x58, 0x5b, 0x70),
    surface1: Color::rgb(0x45, 0x47, 0x5a),
    surface0: Color::rgb(0x31, 0x32, 0x44),
    base: Color::rgb(0x1e, 0x1e, 0x2e),
    mantle: Color::rgb(0x18, 0x18, 0x25),
    crust: Color::rgb(0x11, 0x11, 0x1b),
};
