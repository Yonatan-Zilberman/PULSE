use super::schema::{CREATE_INDEXES, CREATE_TRACKS_TABLE};
use rusqlite::{Connection, Result};

pub struct LibraryCache {
    conn: Connection,
}

impl LibraryCache {
    pub fn in_memory() -> Result<Self> {
        let conn = Connection::open_in_memory()?;
        let cache = Self { conn };
        cache.init_schema()?;
        Ok(cache)
    }

    pub fn open<P: AsRef<std::path::Path>>(path: P) -> Result<Self> {
        let conn = Connection::open(path)?;
        let cache = Self { conn };
        cache.init_schema()?;
        Ok(cache)
    }

    fn init_schema(&self) -> Result<()> {
        self.conn.execute_batch(CREATE_TRACKS_TABLE)?;
        self.conn.execute_batch(CREATE_INDEXES)?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_in_memory_library_cache_initialization() {
        let cache = LibraryCache::in_memory();
        assert!(cache.is_ok(), "LibraryCache failed to initialize in memory");
    }
}
