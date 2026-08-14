use std::collections::HashMap;

#[derive(Default)]
pub struct TransitionGraph {
    // Adjacency mapping: track_id -> [(destination_track_id, transition_cost)]
    pub edges: HashMap<String, Vec<(String, f32)>>,
}

impl TransitionGraph {
    pub fn new() -> Self {
        Self {
            edges: HashMap::new(),
        }
    }

    pub fn add_transition(&mut self, from: String, to: String, cost: f32) {
        self.edges.entry(from).or_default().push((to, cost));
    }
}
