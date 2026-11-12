//! The part of the `PipeWire` graph the privacy indicator reads, and the rules
//! that turn it into "who's recording what" (the classification half of the
//! C bar's `pipewire/pipewire_client.c`). Plain data, no `PipeWire` types,
//! so it's unit-tested without a `PipeWire` connection.

use std::collections::{BTreeMap, HashMap};

/// The bar's own media visualizer captures as a `Stream/Input/Audio` node of
/// this name (M14). It's never counted, or the privacy box would show a mic
/// naming stickBar whenever the media dropdown is open during playback.
pub(crate) const VISUALIZER_NODE_NAME: &str = "stickBar-visualizer";

/// A recording's kind, in the order the dropdown lists them.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(crate) enum Category {
    /// A running `Stream/Input/Audio` node.
    Mic,
    /// A running `Stream/Input/Video` node fed by a camera device.
    Webcam,
    /// A running `Stream/Input/Video` node fed by anything else.
    ScreenShare,
}

/// Who's recording what: one entry per active stream, by category (mic,
/// webcam, screen share), then in node order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct PrivacyState {
    /// (kind, app name) per active stream.
    pub(crate) entries: Vec<(Category, String)>,
}

impl PrivacyState {
    /// Whether any stream is of kind `category`.
    pub(crate) fn has(&self, category: Category) -> bool {
        self.entries.iter().any(|(c, _)| *c == category)
    }
}

/// One node, as far as the indicator cares.
#[derive(Clone, Debug, Default)]
pub(crate) struct NodeEntry {
    /// `media.class` ("" if none).
    pub(crate) media_class: String,
    /// Who it belongs to, for the dropdown ([`display_name`]).
    pub(crate) name: String,
    /// Its state is `Running` (only watched for the streams [`is_watched`] picks).
    pub(crate) running: bool,
}

/// Every node and link, by global id.
#[derive(Debug, Default)]
pub(crate) struct Graph {
    /// Nodes, ordered by id (roughly creation order).
    pub(crate) nodes: BTreeMap<u32, NodeEntry>,
    /// Links: (output node, input node), read off the link's global props.
    pub(crate) links: HashMap<u32, (u32, u32)>,
}

impl Graph {
    /// The active streams, by category then node order.
    pub(crate) fn privacy_state(&self) -> PrivacyState {
        let mut entries: Vec<(Category, String)> = self
            .nodes
            .iter()
            .filter(|(_, node)| node.running)
            .filter_map(|(&id, node)| match node.media_class.as_str() {
                "Stream/Input/Audio" => Some((Category::Mic, node.name.clone())),
                "Stream/Input/Video" if self.is_webcam(id) => {
                    Some((Category::Webcam, node.name.clone()))
                }
                "Stream/Input/Video" => Some((Category::ScreenShare, node.name.clone())),
                _ => None,
            })
            .collect();
        // Stable: within a category, node order is kept.
        entries.sort_by_key(|(category, _)| *category);
        PrivacyState { entries }
    }

    /// Whether the video stream `id` reads a camera, by following the link
    /// into it back to its source: a real camera (v4l2/libcamera) is a
    /// `Video/Source` node by `PipeWire`'s device-monitor convention; anything
    /// else (a portal's screen-capture producer, or no link yet) counts as a
    /// screen share by elimination. **Not verified on real hardware** (no
    /// webcam, no portal session on the development machine): the first
    /// thing to re-check if it's ever wrong (`docs/modules/privacy.md`).
    fn is_webcam(&self, id: u32) -> bool {
        self.links
            .values()
            .find(|&&(_, input)| input == id)
            .and_then(|&(output, _)| self.nodes.get(&output))
            .is_some_and(|source| source.media_class.starts_with("Video/Source"))
    }
}

/// Whether a node of this class and name is a stream whose running state is
/// watched: recordings of audio or video, but not the bar's own visualizer.
pub(crate) fn is_watched(media_class: &str, node_name: &str) -> bool {
    match media_class {
        "Stream/Input/Audio" => node_name != VISUALIZER_NODE_NAME,
        "Stream/Input/Video" => true,
        _ => false,
    }
}

/// The name to show for a node: the first non-empty of `application.name`,
/// `node.description`, `node.name`, `media.name` (what `lookup` gives for
/// each key), else "Unknown app". Real apps set the first (a `parecord`
/// capture showed `application.name=parecord`).
pub(crate) fn display_name<'a>(lookup: impl Fn(&str) -> Option<&'a str>) -> String {
    [
        "application.name",
        "node.description",
        "node.name",
        "media.name",
    ]
    .into_iter()
    .find_map(|key| lookup(key).filter(|v| !v.is_empty()))
    .unwrap_or("Unknown app")
    .to_owned()
}

#[cfg(test)]
mod tests {
    use super::{Category, Graph, NodeEntry, display_name, is_watched};

    /// A node.
    fn node(media_class: &str, name: &str, running: bool) -> NodeEntry {
        NodeEntry {
            media_class: media_class.to_owned(),
            name: name.to_owned(),
            running,
        }
    }

    #[test]
    fn classifies_streams() {
        let mut g = Graph::default();
        g.nodes.extend([
            (30, node("Video/Source", "camera", false)),
            (40, node("Stream/Input/Video", "zoom", true)),
            (41, node("Stream/Input/Video", "obs", true)),
            (50, node("Stream/Input/Audio", "parecord", true)),
            (51, node("Stream/Input/Audio", "idle", false)),
            (52, node("Stream/Output/Audio", "spotify", true)),
            (53, node("Stream/Input/Audio", "second", true)),
        ]);
        g.links.extend([(90, (30, 40))]); // camera -> zoom
        let state = g.privacy_state();
        assert_eq!(
            state.entries,
            vec![
                (Category::Mic, "parecord".to_owned()),
                (Category::Mic, "second".to_owned()),
                (Category::Webcam, "zoom".to_owned()),
                (Category::ScreenShare, "obs".to_owned()),
            ]
        );
        assert!(state.has(Category::Webcam));
        assert_eq!(Graph::default().privacy_state().entries.len(), 0);
    }

    #[test]
    fn watched_streams_and_names() {
        assert!(is_watched("Stream/Input/Audio", "parecord"));
        assert!(!is_watched("Stream/Input/Audio", "stickBar-visualizer"));
        assert!(is_watched("Stream/Input/Video", ""));
        assert!(!is_watched("Audio/Sink", "speakers"));
        let props = [("application.name", ""), ("node.name", "alsa_input")];
        let lookup = |k: &str| props.iter().find(|(key, _)| *key == k).map(|(_, v)| *v);
        assert_eq!(display_name(lookup), "alsa_input");
        assert_eq!(display_name(|_| None), "Unknown app");
    }
}
