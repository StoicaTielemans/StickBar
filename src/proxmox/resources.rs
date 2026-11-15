//! Parsing `GET /api2/json/cluster/resources`: one call that lists every
//! node, VM (`qemu`), container (`lxc`) and storage the token can see, with
//! their live figures. One request per poll instead of one per node and per
//! guest.
//!
//! Field meanings (Proxmox API, `cluster/resources`):
//! - `cpu` is a **fraction** of the object's `maxcpu` cores (0.25 = 25% of
//!   them), not a percentage;
//! - `mem`/`maxmem`, `disk`/`maxdisk` are bytes;
//! - `uptime` is seconds (0 when stopped);
//! - a node's `status` is `online`/`offline`/`unknown`; a guest's is
//!   `running`/`stopped`/`paused`; a storage's is `available` or not;
//! - `template: 1` marks a VM/CT template, which never runs; it's left out.
//!
//! Every field is read with `.get()` and a default, so a field missing from
//! one Proxmox version just shows as 0 instead of failing the whole poll.

use serde_json::Value;

/// One cluster node (normally just the one).
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Node {
    /// Its name (`pve`).
    pub(crate) name: String,
    /// `online` per Proxmox.
    pub(crate) online: bool,
    /// CPU use, 0..=1 of all its cores.
    pub(crate) cpu: f64,
    /// How many cores.
    pub(crate) maxcpu: u64,
    /// RAM used, bytes.
    pub(crate) mem: f64,
    /// RAM total, bytes.
    pub(crate) maxmem: f64,
    /// Root disk used, bytes.
    pub(crate) disk: f64,
    /// Root disk size, bytes.
    pub(crate) maxdisk: f64,
    /// Seconds since boot.
    pub(crate) uptime: u64,
}

/// A VM or a container.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum GuestKind {
    /// A QEMU virtual machine.
    Vm,
    /// An LXC container.
    Ct,
}

/// A guest's power state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum GuestState {
    /// Running.
    Running,
    /// Paused (suspended in RAM).
    Paused,
    /// Stopped, or any state this module doesn't know.
    Stopped,
}

/// One VM or container.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Guest {
    /// Its id (`101`).
    pub(crate) vmid: u64,
    /// Its name.
    pub(crate) name: String,
    /// VM or container.
    pub(crate) kind: GuestKind,
    /// Its power state.
    pub(crate) state: GuestState,
    /// CPU use, 0..=1 of its own `maxcpu` cores.
    pub(crate) cpu: f64,
    /// How many cores it has.
    pub(crate) maxcpu: u64,
    /// RAM used, bytes.
    pub(crate) mem: f64,
    /// RAM it's allowed, bytes.
    pub(crate) maxmem: f64,
}

/// One storage.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Storage {
    /// Its name (`local-lvm`).
    pub(crate) name: String,
    /// Whether it's `available`.
    pub(crate) available: bool,
    /// Used, bytes.
    pub(crate) disk: f64,
    /// Size, bytes.
    pub(crate) maxdisk: f64,
}

/// Everything one poll found.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Cluster {
    /// Nodes, by name.
    pub(crate) nodes: Vec<Node>,
    /// VMs and containers (templates left out), by id.
    pub(crate) guests: Vec<Guest>,
    /// Storages, by name.
    pub(crate) storages: Vec<Storage>,
}

impl Cluster {
    /// How many guests are running (paused ones count: they hold their RAM).
    pub(crate) fn running(&self) -> usize {
        self.guests
            .iter()
            .filter(|g| g.state != GuestState::Stopped)
            .count()
    }
}

/// Parses the response body. An error means the body isn't the API's
/// `{"data": [...]}`. A token that may see nothing gets an empty `data` (not
/// an error from Proxmox), which is reported here as one, since a cluster
/// without a node is never real.
pub(crate) fn parse(body: &str) -> Result<Cluster, String> {
    let root: Value = serde_json::from_str(body).map_err(|e| format!("bad JSON: {e}"))?;
    let items = root
        .get("data")
        .and_then(Value::as_array)
        .ok_or("no `data` list in the answer")?;
    let mut cluster = Cluster::default();
    for item in items {
        match str_field(item, "type") {
            "node" => cluster.nodes.push(Node {
                name: str_field(item, "node").to_owned(),
                online: str_field(item, "status") == "online",
                cpu: f64_field(item, "cpu"),
                maxcpu: u64_field(item, "maxcpu"),
                mem: f64_field(item, "mem"),
                maxmem: f64_field(item, "maxmem"),
                disk: f64_field(item, "disk"),
                maxdisk: f64_field(item, "maxdisk"),
                uptime: u64_field(item, "uptime"),
            }),
            kind @ ("qemu" | "lxc") => {
                if u64_field(item, "template") == 1 {
                    continue;
                }
                cluster.guests.push(Guest {
                    vmid: u64_field(item, "vmid"),
                    name: str_field(item, "name").to_owned(),
                    kind: if kind == "qemu" {
                        GuestKind::Vm
                    } else {
                        GuestKind::Ct
                    },
                    state: match str_field(item, "status") {
                        "running" => GuestState::Running,
                        "paused" => GuestState::Paused,
                        _ => GuestState::Stopped,
                    },
                    cpu: f64_field(item, "cpu"),
                    maxcpu: u64_field(item, "maxcpu"),
                    mem: f64_field(item, "mem"),
                    maxmem: f64_field(item, "maxmem"),
                });
            }
            "storage" => cluster.storages.push(Storage {
                name: str_field(item, "storage").to_owned(),
                available: str_field(item, "status") == "available",
                disk: f64_field(item, "disk"),
                maxdisk: f64_field(item, "maxdisk"),
            }),
            // `sdn`, `pool` and whatever later versions add.
            _ => {}
        }
    }
    if cluster.nodes.is_empty() {
        return Err("the token sees no node (give it the PVEAuditor role on /)".to_owned());
    }
    cluster.nodes.sort_by(|a, b| a.name.cmp(&b.name));
    cluster.guests.sort_by_key(|g| g.vmid);
    cluster.storages.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(cluster)
}

/// A string field, or `""`.
fn str_field<'v>(item: &'v Value, key: &str) -> &'v str {
    item.get(key).and_then(Value::as_str).unwrap_or_default()
}

/// A number field as `f64` (byte counts, fractions), or 0.
fn f64_field(item: &Value, key: &str) -> f64 {
    item.get(key).and_then(Value::as_f64).unwrap_or_default()
}

/// A whole-number field, or 0.
fn u64_field(item: &Value, key: &str) -> u64 {
    item.get(key).and_then(Value::as_u64).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::{GuestKind, GuestState, parse};

    /// Shaped like a real Proxmox VE 9 answer (trimmed fields).
    const BODY: &str = r#"{"data":[
        {"id":"qemu/101","type":"qemu","vmid":101,"name":"homeassistant","node":"pve",
         "status":"running","cpu":0.0312,"maxcpu":2,"mem":2147483648,"maxmem":4294967296,
         "uptime":3600,"template":0},
        {"id":"node/pve","type":"node","node":"pve","status":"online","cpu":0.12,
         "maxcpu":8,"mem":8589934592,"maxmem":34359738368,"disk":1.0e10,
         "maxdisk":1.0e11,"uptime":273600},
        {"id":"lxc/100","type":"lxc","vmid":100,"name":"pihole","node":"pve",
         "status":"stopped","cpu":0,"maxcpu":1,"mem":0,"maxmem":536870912,"uptime":0},
        {"id":"qemu/9000","type":"qemu","vmid":9000,"name":"tmpl","status":"stopped",
         "template":1},
        {"id":"storage/pve/local-lvm","type":"storage","storage":"local-lvm","node":"pve",
         "status":"available","disk":5.0e10,"maxdisk":2.0e11},
        {"id":"sdn/pve/localnetwork","type":"sdn","sdn":"localnetwork","status":"ok"}
    ]}"#;

    #[test]
    fn parses_nodes_guests_and_storage() {
        let cluster = parse(BODY).unwrap();
        assert_eq!(cluster.nodes.len(), 1);
        let node = cluster.nodes.first().unwrap();
        assert!(node.online && node.maxcpu == 8 && node.uptime == 273_600);
        // Sorted by id; the template is left out.
        let ids: Vec<u64> = cluster.guests.iter().map(|g| g.vmid).collect();
        assert_eq!(ids, [100, 101]);
        let ha = cluster.guests.last().unwrap();
        assert_eq!((ha.kind, ha.state), (GuestKind::Vm, GuestState::Running));
        assert_eq!(cluster.guests.first().unwrap().kind, GuestKind::Ct);
        assert_eq!(cluster.running(), 1);
        assert_eq!(cluster.storages.len(), 1);
    }

    /// Whether `body` fails to parse.
    fn fails(body: &str) -> bool {
        parse(body).is_err()
    }

    #[test]
    fn an_answer_without_a_node_is_an_error() {
        assert!(fails(r#"{"data":[]}"#));
        assert!(fails(r#"{"errors":"x"}"#));
        assert!(fails("<html>"));
    }
}
