#include "stdafx.h"
#include "MapDatabase.hpp"
#include "Database.hpp"
#include "Beatmap.hpp"
#include "TinySHA1.hpp"
#include "Shared/Profiling.hpp"
#include "Shared/Files.hpp"
#include "Shared/Time.hpp"
#include <thread>
#include <mutex>
#include <chrono>
using std::thread;
using std::mutex;
using namespace std;

class MapDatabase_Impl
{
public:
	// For calling delegates
	MapDatabase& m_outer;

	thread m_thread;
	bool m_searching = false;
	bool m_interruptSearch = false;
	Set<String> m_searchPaths;
	Database m_database;

	Map<int32, FolderIndex*> m_folders;
	Map<int32, ChartIndex*> m_charts;
	Map<String, ChartIndex*> m_chartsByHash;
	Map<String, FolderIndex*> m_foldersByPath;
	int32 m_nextFolderId = 1;
	int32 m_nextChartId = 1;
	String m_sortField = "title";

	struct SearchState
	{
		struct ExistingDifficulty
		{
			int32 id;
			uint64 lwt;
		};
		// Maps file paths to the id's and last write time's for difficulties already in the database
		Map<String, ExistingDifficulty> difficulties;
	} m_searchState;

	// Represents an event produced from a scan
	//	a difficulty can be removed/added/updated
	//	a BeatmapSettings structure will be provided for added/updated events
	struct Event
	{
		enum Action
		{
			Added,
			Removed,
			Updated
		};
		Action action;
		String path;
		// Current lwt of file
		uint64 lwt;
		// Id of the map
		int32 id;
		// Scanned map data, for added/updated maps
		BeatmapSettings* mapData = nullptr;
		String hash;
	};
	List<Event> m_pendingChanges;
	mutex m_pendingChangesLock;

	static const int32 m_version = 13;

public:
	MapDatabase_Impl(MapDatabase& outer) : m_outer(outer)
	{
		String databasePath = Path::Absolute("maps.db");
		if(!m_database.Open(databasePath))
		{
			Logf("Failed to open database [%s]", Logger::Warning, databasePath);
			assert(false);
		}

		bool rebuild = false;
		bool update = false;
		DBStatement versionQuery = m_database.Query("SELECT version FROM `Database`");
		int32 gotVersion = 0;
		if(versionQuery && versionQuery.Step())
		{
			gotVersion = versionQuery.IntColumn(0);
			if(gotVersion != m_version)
			{
				update = true;
			}
		}
		else
		{
			// Create DB 
			m_database.Exec("DROP TABLE IF EXISTS Database");
			m_database.Exec("CREATE TABLE Database(version INTEGER)");
			m_database.Exec(Utility::Sprintf("INSERT OR REPLACE INTO Database(rowid, version) VALUES(1, %d)", m_version));
			rebuild = true;
		}
		versionQuery.Finish();

		if(rebuild)
		{
			m_CreateTables();

			// Update database version
			m_database.Exec(Utility::Sprintf("UPDATE Database SET `version`=%d WHERE `rowid`=1", m_version));
		}
		else if (update)
		{
			ProfilerScope $(Utility::Sprintf("Upgrading db (%d -> %d)", gotVersion, m_version));

			m_outer.OnDatabaseUpdateStarted.Call(1);


			///TODO: Make loop for doing iterative upgrades
			if (gotVersion == 8)  //upgrade from 8 to 9
			{
				m_database.Exec("ALTER TABLE Scores ADD COLUMN hitstats BLOB");
				gotVersion = 9;
			}
			if (gotVersion == 9)  //upgrade from 9 to 10
			{
				m_database.Exec("ALTER TABLE Scores ADD COLUMN timestamp INTEGER");
				gotVersion = 10;
			}
			if (gotVersion == 10)  //upgrade from 10 to 11
			{
				m_database.Exec("ALTER TABLE Difficulties ADD COLUMN hash TEXT");
				gotVersion = 11;
			}
			if (gotVersion == 11) //upgrade from 11 to 12
			{
				m_database.Exec("CREATE TABLE Collections"
					"(collection TEXT, mapid INTEGER, "
					"UNIQUE(collection,mapid), "
					"FOREIGN KEY(mapid) REFERENCES Maps(rowid))");
				gotVersion = 12;
			}
			if (gotVersion == 12) //upgrade from 12 to 13
			{
				//back up old db file
				Path::Copy(Path::Absolute("maps.db"), Path::Absolute("maps.db_" + Shared::Time::Now().ToString() + ".bak"));

				int diffCount = 1;
				{
					// Do in its own scope so that it will destruct before we modify anything else
					DBStatement diffCountStmt = m_database.Query("SELECT COUNT(rowid) FROM Difficulties");
					if (diffCountStmt.StepRow())
					{
						diffCount = diffCountStmt.IntColumn(0);
					}
				}
				m_outer.OnDatabaseUpdateProgress.Call(0, diffCount);

				int progress = 0;
				int totalScoreCount = 0;

				DBStatement diffScan = m_database.Query("SELECT rowid,path FROM Difficulties");

				Vector<ScoreIndex> scoresToAdd;
				while (diffScan.StepRow())
				{
					if (progress % 16 == 0)
						m_outer.OnDatabaseUpdateProgress.Call(progress, diffCount);
					progress++;

					bool noScores = true;
					int diffid = diffScan.IntColumn(0);
					String diffpath = diffScan.StringColumn(1);
					DBStatement scoreCount = m_database.Query("SELECT COUNT(*) FROM scores WHERE diffid=?");
					scoreCount.BindInt(1, diffid);
					if (scoreCount.StepRow())
					{
						noScores = scoreCount.IntColumn(0) == 0;
					}

					if (noScores)
					{
						continue;
					}


					String hash;
					File diffFile;
					if (diffFile.OpenRead(diffpath))
					{
						char data_buffer[0x80];
						uint32_t digest[5];
						sha1::SHA1 s;

						size_t amount_read = 0;
						size_t read_size;
						do
						{
							read_size = diffFile.Read(data_buffer, sizeof(data_buffer));
							amount_read += read_size;
							s.processBytes(data_buffer, read_size);
						} while (read_size != 0);

						s.getDigest(digest);
						hash = Utility::Sprintf("%08x%08x%08x%08x%08x", digest[0], digest[1], digest[2], digest[3], digest[4]);
					}
					else {
						Logf("Could not open chart file at \"%s\" scores will be lost.", Logger::Warning, diffpath);
						continue;
					}


					DBStatement scoreScan = m_database.Query("SELECT rowid,score,crit,near,miss,gauge,gameflags,hitstats,timestamp,diffid FROM Scores WHERE diffid=?");
					scoreScan.BindInt(1, diffid);
					while (scoreScan.StepRow())
					{
						ScoreIndex score;
						score.id = scoreScan.IntColumn(0);
						score.score = scoreScan.IntColumn(1);
						score.crit = scoreScan.IntColumn(2);
						score.almost = scoreScan.IntColumn(3);
						score.miss = scoreScan.IntColumn(4);
						score.gauge = scoreScan.DoubleColumn(5);
						score.gameflags = scoreScan.IntColumn(6);
						Buffer hitstats = scoreScan.BlobColumn(7);
						score.timestamp = scoreScan.Int64Column(8);
						auto timestamp = Shared::Time(score.timestamp);
						score.chartHash = hash;
						score.replayPath = Path::Normalize(Path::Absolute("replays/" + hash + "/" + timestamp.ToString() + ".urf"));
						Path::CreateDir(Path::Absolute("replays/" + hash));
						File replayFile;
						if (replayFile.OpenWrite(score.replayPath))
						{
							replayFile.Write(hitstats.data(), hitstats.size());
						}
						else
						{
							Logf("Could not open replay file at \"%s\" replay data will be lost.", Logger::Warning, score.replayPath);
						}
						scoresToAdd.Add(score);
						totalScoreCount++;
					}

				}

				progress = 0;

				m_database.Exec("DROP TABLE IF EXISTS Maps");
				m_database.Exec("DROP TABLE IF EXISTS Difficulties");
				m_CreateTables();

				DBStatement addScore = m_database.Query("INSERT INTO Scores(score,crit,near,miss,gauge,gameflags,replay,timestamp,chart_hash) VALUES(?,?,?,?,?,?,?,?,?)");
				
				m_database.Exec("BEGIN");
				for (ScoreIndex& score : scoresToAdd)
				{
					addScore.BindInt(1, score.score);
					addScore.BindInt(2, score.crit);
					addScore.BindInt(3, score.almost);
					addScore.BindInt(4, score.miss);
					addScore.BindDouble(5, score.gauge);
					addScore.BindInt(6, score.gameflags);
					addScore.BindString(7, score.replayPath);
					addScore.BindInt64(8, score.timestamp);
					addScore.BindString(9, score.chartHash);

					addScore.Step();
					addScore.Rewind();

					if (progress % 16 == 0)
					{
						m_outer.OnDatabaseUpdateProgress.Call(progress, totalScoreCount);
					}
					progress++;
				}
				m_database.Exec("END");
				m_database.Exec("VACUUM");
			}
			m_database.Exec(Utility::Sprintf("UPDATE Database SET `version`=%d WHERE `rowid`=1", m_version));

			m_outer.OnDatabaseUpdateDone.Call();
		}
		else
		{
			// Load initial folder tree
			m_LoadInitialData();
		}
	}
	~MapDatabase_Impl()
	{
		StopSearching();
		m_CleanupMapIndex();

		//discard pending changes, probably should apply them(?)
		auto changes = FlushChanges();
		for (auto& c : changes)
		{
			if(c.mapData)
				delete c.mapData;
		}
	}

	void StartSearching()
	{
		if(m_searching)
			return;

		if(m_thread.joinable())
			m_thread.join();
		// Apply previous diff to prevent duplicated entry 
		Update();
		// Create initial data set to compare to when evaluating if a file is added/removed/updated
		m_LoadInitialData();
		m_interruptSearch = false;
		m_searching = true;
		m_thread = thread(&MapDatabase_Impl::m_SearchThread, this);
	}
	void StopSearching()
	{
		m_interruptSearch = true;
		m_searching = false;
		if(m_thread.joinable())
		{
			m_thread.join();
		}
	}
	void AddSearchPath(const String& path)
	{
		String normalizedPath = Path::Normalize(Path::Absolute(path));
		if(m_searchPaths.Contains(normalizedPath))
			return;

		m_searchPaths.Add(normalizedPath);
	}
	void RemoveSearchPath(const String& path)
	{
		String normalizedPath = Path::Normalize(Path::Absolute(path));
		if(!m_searchPaths.Contains(normalizedPath))
			return;

		m_searchPaths.erase(normalizedPath);
	}

	/* Thread safe event queue functions */
	// Add a new change to the change queue
	void AddChange(Event change)
	{
		m_pendingChangesLock.lock();
		m_pendingChanges.emplace_back(change);
		m_pendingChangesLock.unlock();
	}
	// Removes changes from the queue and returns them
	//	additionally you can specify the maximum amount of changes to remove from the queue
	List<Event> FlushChanges(size_t maxChanges = -1)
	{
		List<Event> changes;
		m_pendingChangesLock.lock();
		if(maxChanges == -1)
		{
			changes = std::move(m_pendingChanges); // All changes
		}
		else
		{
			for(size_t i = 0; i < maxChanges && !m_pendingChanges.empty(); i++)
			{
				changes.AddFront(m_pendingChanges.front());
				m_pendingChanges.pop_front();
			}
		}
		m_pendingChangesLock.unlock();
		return std::move(changes);
	}

	Map<int32, FolderIndex*> FindFoldersByHash(const String& hash)
	{
		String stmt = "SELECT DISTINCT folderId FROM Charts WHERE hash = ?";
		DBStatement search = m_database.Query(stmt);
		search.BindString(1, hash);

		Map<int32, FolderIndex*> res;
		while (search.StepRow())
		{
			int32 id = search.IntColumn(0);
			FolderIndex** folder = m_folders.Find(id);
			if (folder)
			{
				res.Add(id, *folder);
			}
		}

		return res;
	}

	Map<int32, FolderIndex*> FindFoldersByPath(const String& searchString)
	{
		String stmt = "SELECT DISTINCT folderId FROM Charts WHERE path LIKE \"%" + searchString + "%\"";

		Map<int32, FolderIndex*> res;
		DBStatement search = m_database.Query(stmt);
		while(search.StepRow())
		{
			int32 id = search.IntColumn(0);
			FolderIndex** folder = m_folders.Find(id);
			if(folder)
			{
				res.Add(id, *folder);
			}
		}

		return res;
	}
	
	Map<int32, FolderIndex*> FindFolders(const String& searchString)
	{
		WString test = Utility::ConvertToWString(searchString);
		String stmt = "SELECT DISTINCT folderId FROM Charts WHERE";


		//search.spl
		Vector<String> terms = searchString.Explode(" ");
		int32 i = 0;
		for(auto term : terms)
		{
			if(i > 0)
				stmt += " AND";
			stmt += " (artist LIKE \"%" + term + "%\"" + 
				" OR title LIKE \"%" + term + "%\"" +
				" OR path LIKE \"%" + term + "%\"" +
				" OR effector LIKE \"%" + term + "%\"" +
				" OR artist_translit LIKE \"%" + term + "%\"" +
				" OR title_translit LIKE \"%" + term + "%\")";
			i++;
		}

		Map<int32, FolderIndex*> res;
		DBStatement search = m_database.Query(stmt);
		while(search.StepRow())
		{
			int32 id = search.IntColumn(0);
			FolderIndex** folder = m_folders.Find(id);
			if(folder)
			{
				res.Add(id, *folder);
			}
		}


		return res;
	}

	Vector<String> GetCollections()
	{
		Vector<String> res;
		DBStatement search = m_database.Query("SELECT DISTINCT collection FROM collections");
		while (search.StepRow())
		{
			res.Add(search.StringColumn(0));
		}
		return res;
	}

	Vector<String> GetCollectionsForMap(int32 mapid)
	{
		Vector<String> res;
		DBStatement search = m_database.Query(Utility::Sprintf("SELECT DISTINCT collection FROM collections WHERE folderid==%d", mapid));
		while (search.StepRow())
		{
			res.Add(search.StringColumn(0));
		}
		return res;
	}

	Map<int32, FolderIndex*> FindFoldersByCollection(const String& collection)
	{
		String stmt = Utility::Sprintf("SELECT folderid FROM Collections WHERE collection==\"%s\"", collection);

		Map<int32, FolderIndex*> res;
		DBStatement search = m_database.Query(stmt);
		while (search.StepRow())
		{
			int32 id = search.IntColumn(0);
			FolderIndex** folder = m_folders.Find(id);
			if (folder)
			{
				res.Add(id, *folder);
			}
		}

		return res;
	}


	Map<int32, FolderIndex*> FindFoldersByFolder(const String& folder)
	{
		char csep[2];
		csep[0] = Path::sep;
		csep[1] = 0;
		String sep(csep);
		String stmt = "SELECT rowid FROM folders WHERE path LIKE \"%" + sep + folder + sep + "%\"";

		Map<int32, FolderIndex*> res;
		DBStatement search = m_database.Query(stmt);
		while (search.StepRow())
		{
			int32 id = search.IntColumn(0);
			FolderIndex** folder = m_folders.Find(id);
			if (folder)
			{
				res.Add(id, *folder);
			}
		}

		return res;
	}
	// Processes pending database changes
	void Update()
	{
		List<Event> changes = FlushChanges();
		if(changes.empty())
			return;

		DBStatement addChart = m_database.Query("INSERT INTO Charts("
			"folderId,path,title,artist,title_translit,artist_translit,jacket_path,effector,illustrator,"
			"diff_name,diff_shortname,bpm,diff_index,level,hash,preview_file,preview_offset,preview_length,lwt) "
			"VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
		DBStatement addFolder = m_database.Query("INSERT INTO Folders(path,rowid) VALUES(?,?)");
		DBStatement update = m_database.Query("UPDATE Charts SET path=?,title=?,artist=?,title_translit=?,artist_translit=?,jacket_path=?,effector=?,illustrator=?,"
			"diff_name=?,diff_shortname=?,bpm=?,diff_index=?,level=?,hash=?,preview_file=?,preview_offset=?,preview_length=?,lwt=? WHERE rowid=?"); //TODO: update
		DBStatement removeChart = m_database.Query("DELETE FROM Charts WHERE rowid=?");
		DBStatement removeFolder = m_database.Query("DELETE FROM Folders WHERE rowid=?");
		DBStatement scoreScan = m_database.Query("SELECT rowid,score,crit,near,miss,gauge,gameflags,replay,timestamp FROM Scores WHERE chart_hash=?");

		Set<FolderIndex*> addedEvents;
		Set<FolderIndex*> removeEvents;
		Set<FolderIndex*> updatedEvents;

		const String diffShortNames[4] = { "NOV", "ADV", "EXH", "INF" };
		const String diffNames[4] = { "Novice", "Advanced", "Exhaust", "Infinite" };

		m_database.Exec("BEGIN");
		for(Event& e : changes)
		{
			if(e.action == Event::Added)
			{
				String folderPath = Path::RemoveLast(e.path, nullptr);
				bool existingUpdated;
				FolderIndex* folder;

				// Add or get folder
				auto folderIt = m_foldersByPath.find(folderPath);
				if(folderIt == m_foldersByPath.end())
				{
					// Add folder
					folder = new FolderIndex();
					folder->id = m_nextFolderId++;
					folder->path = folderPath;
					folder->selectId = m_folders.size();

					m_folders.Add(folder->id, folder);
					m_foldersByPath.Add(folder->path, folder);

					addFolder.BindString(1, folder->path);
					addFolder.BindInt(2, folder->id);
					addFolder.Step();
					addFolder.Rewind();

					existingUpdated = false; // New folder
				}
				else
				{
					folder = folderIt->second;
					existingUpdated = true; // Existing folder
				}


				ChartIndex* chart = new ChartIndex();
				chart->id = m_nextChartId++;
				chart->lwt = e.lwt;
				chart->folderId = folder->id;
				chart->path = e.path;
				chart->title = e.mapData->title;
				chart->artist = e.mapData->artist;
				chart->level = e.mapData->level;
				chart->effector = e.mapData->effector;
				chart->preview_file = e.mapData->audioNoFX;
				chart->preview_offset = e.mapData->previewOffset;
				chart->preview_length = e.mapData->previewDuration;
				chart->diff_index = e.mapData->difficulty;
				chart->diff_name = diffNames[e.mapData->difficulty];
				chart->diff_shortname = diffShortNames[e.mapData->difficulty];
				chart->bpm = e.mapData->bpm;
				chart->illustrator = e.mapData->illustrator;
				chart->jacket_path = e.mapData->jacketPath;
				chart->hash = e.hash;

				// Check for existing scores for this chart
				scoreScan.BindString(1, chart->hash);
				while (scoreScan.StepRow())
				{
					ScoreIndex* score = new ScoreIndex();
					score->id = scoreScan.IntColumn(0);
					score->score = scoreScan.IntColumn(1);
					score->crit = scoreScan.IntColumn(2);
					score->almost = scoreScan.IntColumn(3);
					score->miss = scoreScan.IntColumn(4);
					score->gauge = scoreScan.DoubleColumn(5);
					score->gameflags = scoreScan.IntColumn(6);
					score->replayPath = scoreScan.StringColumn(7);

					score->timestamp = scoreScan.Int64Column(8);
					score->chartHash = chart->hash;
					chart->scores.Add(score);
				}
				scoreScan.Rewind();

				m_SortScores(chart);


				m_charts.Add(chart->id, chart);
				m_chartsByHash.Add(chart->hash, chart);
				// Add diff to map and resort
				folder->charts.Add(chart);
				m_SortCharts(folder);

				// Add Chart
				addChart.BindInt(1, chart->folderId);
				addChart.BindString(2, chart->path);
				addChart.BindString(3, chart->title);
				addChart.BindString(4, chart->artist);
				addChart.BindString(5, chart->title_translit);
				addChart.BindString(6, chart->artist_translit);
				addChart.BindString(7, chart->jacket_path);
				addChart.BindString(8, chart->effector);
				addChart.BindString(9, chart->illustrator);
				addChart.BindString(10, chart->diff_name);
				addChart.BindString(11, chart->diff_shortname);
				addChart.BindString(12, chart->bpm);
				addChart.BindInt(13, chart->diff_index);
				addChart.BindInt(14, chart->level);
				addChart.BindString(15, chart->hash);
				addChart.BindString(16, chart->preview_file);
				addChart.BindInt(17, chart->preview_offset);
				addChart.BindInt(18, chart->preview_length);
				addChart.BindInt64(19, chart->lwt);

				addChart.Step();
				addChart.Rewind();

				// Send appropriate notification
				if(existingUpdated)
				{
					updatedEvents.Add(folder);
				}
				else
				{
					addedEvents.Add(folder);
				}
			}
			else if(e.action == Event::Updated)
			{
				update.BindString(1, e.path);
				update.BindString(2, e.mapData->title);
				update.BindString(3, e.mapData->artist);
				update.BindString(4, "");
				update.BindString(5, "");
				update.BindString(6, e.mapData->jacketPath);
				update.BindString(7, e.mapData->effector);
				update.BindString(8, e.mapData->illustrator);
				update.BindString(9, diffNames[e.mapData->difficulty]);
				update.BindString(10, diffShortNames[e.mapData->difficulty]);
				update.BindString(11, e.mapData->bpm);
				update.BindInt(12, e.mapData->difficulty);
				update.BindInt(13, e.mapData->level);
				update.BindString(14, e.hash);
				update.BindString(15, e.mapData->audioNoFX);
				update.BindInt(16, e.mapData->previewOffset);
				update.BindInt(17, e.mapData->previewDuration);
				update.BindInt64(18, e.lwt);
				update.BindInt(19, e.id);

				update.Step();
				update.Rewind();

				auto itChart = m_charts.find(e.id);
				assert(itChart != m_charts.end());

				ChartIndex* chart = itChart->second;
				chart->lwt = e.lwt;
				chart->path = e.path;
				chart->title = e.mapData->title;
				chart->artist = e.mapData->artist;
				chart->level = e.mapData->level;
				chart->effector = e.mapData->effector;
				chart->preview_file = e.mapData->audioNoFX;
				chart->preview_offset = e.mapData->previewOffset;
				chart->preview_length = e.mapData->previewDuration;
				chart->diff_index = e.mapData->difficulty;
				chart->diff_name = diffNames[e.mapData->difficulty];
				chart->diff_shortname = diffShortNames[e.mapData->difficulty];
				chart->bpm = e.mapData->bpm;
				chart->illustrator = e.mapData->illustrator;
				chart->jacket_path = e.mapData->jacketPath;
				chart->hash = e.hash;


				auto itFolder = m_folders.find(chart->folderId);
				assert(itFolder != m_folders.end());

				// Send notification
				updatedEvents.Add(itFolder->second);
			}
			else if(e.action == Event::Removed)
			{
				auto itChart = m_charts.find(e.id);
				assert(itChart != m_charts.end());

				auto itFolder = m_folders.find(itChart->second->folderId);
				assert(itFolder != m_folders.end());

				itFolder->second->charts.Remove(itChart->second);

				for (auto s : itChart->second->scores)
				{
					delete s;
				}
				itChart->second->scores.clear();
				delete itChart->second;
				m_charts.erase(e.id);

				// Remove diff in db
				removeChart.BindInt(1, e.id);
				removeChart.Step();
				removeChart.Rewind();

				if(itFolder->second->charts.empty()) // Remove map as well
				{
					removeEvents.Add(itFolder->second);

					removeFolder.BindInt(1, itFolder->first);
					removeFolder.Step();
					removeFolder.Rewind();

					m_foldersByPath.erase(itFolder->second->path);
					m_folders.erase(itFolder);
				}
				else
				{
					updatedEvents.Add(itFolder->second);
				}
			}
			if(e.mapData)
				delete e.mapData;
		}
		m_database.Exec("END");

		// Fire events
		if(!removeEvents.empty())
		{
			Vector<FolderIndex*> eventsArray;
			for(auto i : removeEvents)
			{
				// Don't send 'updated' or 'added' events for removed maps
				addedEvents.erase(i);
				updatedEvents.erase(i);
				eventsArray.Add(i);
			}

			m_outer.OnFoldersRemoved.Call(eventsArray);
			for(auto e : eventsArray)
			{
				delete e;
			}
		}
		if(!addedEvents.empty())
		{
			Vector<FolderIndex*> eventsArray;
			for(auto i : addedEvents)
			{
				// Don't send 'updated' events for added maps
				updatedEvents.erase(i);
				eventsArray.Add(i);
			}

			m_outer.OnFoldersAdded.Call(eventsArray);
		}
		if(!updatedEvents.empty())
		{
			Vector<FolderIndex*> eventsArray;
			for(auto i : updatedEvents)
			{
				eventsArray.Add(i);
			}

			m_outer.OnFoldersUpdated.Call(eventsArray);
		}
	}

	void AddScore(const ChartIndex& chart, int score, int crit, int almost, int miss, float gauge, uint32 gameflags, Vector<SimpleHitStat> simpleHitStats, uint64 timestamp)
	{
		DBStatement addScore = m_database.Query("INSERT INTO Scores(score,crit,near,miss,gauge,gameflags,replay,timestamp,chart_hash) VALUES(?,?,?,?,?,?,?,?,?)");
		Path::CreateDir(Path::Absolute("replays/" + chart.hash));
		String replayPath = Path::Normalize(Path::Absolute( "replays/" + chart.hash + "/" + Shared::Time::Now().ToString() + ".urf"));
		File replayFile;
		
		if (replayFile.OpenWrite(replayPath))
		{
			FileWriter fw(replayFile);
			fw.SerializeObject(simpleHitStats);
		}

		m_database.Exec("BEGIN");

		

		addScore.BindInt(1, score);
		addScore.BindInt(2, crit);
		addScore.BindInt(3, almost);
		addScore.BindInt(4, miss);
		addScore.BindDouble(5, gauge);
		addScore.BindInt(6, gameflags);
		addScore.BindString(7, replayPath);
		addScore.BindInt64(8, timestamp);
		addScore.BindString(9, chart.hash);

		addScore.Step();
		addScore.Rewind();

		m_database.Exec("END");

	}

	void AddOrRemoveToCollection(const String& name, int32 mapid)
	{
		DBStatement addColl = m_database.Query("INSERT INTO Collections(folderid,collection) VALUES(?,?)");
		m_database.Exec("BEGIN");

		addColl.BindInt(1, mapid);
		addColl.BindString(2, name);

		bool result = addColl.Step();
		addColl.Rewind();

		m_database.Exec("END");

		if (!result) //Failed to add, try to remove
		{
			m_database.Exec(Utility::Sprintf("DELETE FROM collections WHERE folderid==%d AND collection==\"%s\"", mapid, name));
		}
	}

	ChartIndex* GetRandomChart()
	{
		auto it = m_charts.begin();
		uint32 selection = Random::IntRange(0, (int32)m_charts.size() - 1);
		std::advance(it, selection);
		return it->second;
	}

private:
	void m_CleanupMapIndex()
	{
		for(auto m : m_folders)
		{
			delete m.second;
		}
		for(auto m : m_charts)
		{
			for (auto s : m.second->scores)
			{
				delete s;
			}
			m.second->scores.clear();
			delete m.second;
		}
		m_folders.clear();
		m_charts.clear();
	}
	void m_CreateTables()
	{
		m_database.Exec("DROP TABLE IF EXISTS Folders");
		m_database.Exec("DROP TABLE IF EXISTS Charts");
		m_database.Exec("DROP TABLE IF EXISTS Scores");
		m_database.Exec("DROP TABLE IF EXISTS Collections");

		m_database.Exec("CREATE TABLE Folders"
			"(path TEXT)");

		m_database.Exec("CREATE TABLE Charts"
			"(folderid INTEGER,"
			"title TEXT,"
			"artist TEXT,"
			"title_translit TEXT,"
			"artist_translit TEXT,"
			"jacket_path TEXT,"
			"effector TEXT,"
			"illustrator TEXT,"
			"diff_name TEXT,"
			"diff_shortname TEXT,"
			"path TEXT,"
			"bpm TEXT,"
			"diff_index INTEGER,"
			"level INTEGER,"
			"preview_offset INTEGER,"
			"preview_length INTEGER,"
			"lwt INTEGER,"
			"hash TEXT,"
			"preview_file TEXT,"
			"FOREIGN KEY(folderid) REFERENCES folders(rowid))");

		m_database.Exec("CREATE TABLE Scores"
			"(score INTEGER,"
			"crit INTEGER,"
			"near INTEGER,"
			"miss INTEGER,"
			"gauge REAL,"
			"gameflags INTEGER,"
			"timestamp INTEGER,"
			"replay TEXT,"
			"chart_hash TEXT)");

		m_database.Exec("CREATE TABLE Collections"
			"(collection TEXT, folderid INTEGER, "
			"UNIQUE(collection,folderid), "
			"FOREIGN KEY(folderid) REFERENCES Folders(rowid))");
	}
	void m_LoadInitialData()
	{
		assert(!m_searching);

		// Clear search state
		m_searchState.difficulties.clear();

		// Scan original maps
		m_CleanupMapIndex();

		// Select Maps
		DBStatement mapScan = m_database.Query("SELECT rowid, path FROM Folders");
		while(mapScan.StepRow())
		{
			FolderIndex* folder = new FolderIndex();
			folder->id = mapScan.IntColumn(0);
			folder->path = mapScan.StringColumn(1);
			folder->selectId = m_folders.size();
			m_folders.Add(folder->id, folder);
			m_foldersByPath.Add(folder->path, folder);
		}
		m_nextFolderId = m_folders.empty() ? 1 : (m_folders.rbegin()->first + 1);

		// Select Difficulties
		DBStatement chartScan = m_database.Query("SELECT rowid"
			",folderId"
			",path"
			",title"
			",artist"
			",title_translit"
			",artist_translit"
			",jacket_path"
			",effector"
			",illustrator"
			",diff_name"
			",diff_shortname"
			",bpm"
			",diff_index"
			",level"
			",hash"
			",preview_file"
			",preview_offset"
			",preview_length"
			",lwt "
			"FROM Charts");
		while(chartScan.StepRow())
		{
			ChartIndex* chart = new ChartIndex();
			chart->id = chartScan.IntColumn(0);
			chart->folderId = chartScan.IntColumn(1);
			chart->path = chartScan.StringColumn(2);
			chart->title = chartScan.StringColumn(3);
			chart->artist = chartScan.StringColumn(4);
			chart->title_translit = chartScan.StringColumn(5);
			chart->artist_translit = chartScan.StringColumn(6);
			chart->jacket_path = chartScan.StringColumn(7);
			chart->effector = chartScan.StringColumn(8);
			chart->illustrator = chartScan.StringColumn(9);
			chart->diff_name = chartScan.StringColumn(10);
			chart->diff_shortname = chartScan.StringColumn(11);
			chart->bpm = chartScan.StringColumn(12);
			chart->diff_index = chartScan.IntColumn(13);
			chart->level = chartScan.IntColumn(14);
			chart->hash = chartScan.StringColumn(15);
			chart->preview_file = chartScan.StringColumn(16);
			chart->preview_offset = chartScan.IntColumn(17);
			chart->preview_length = chartScan.IntColumn(18);
			chart->lwt = chartScan.Int64Column(19);

			// Add existing diff
			m_charts.Add(chart->id, chart);
			m_chartsByHash.Add(chart->hash, chart);

			// Add difficulty to map and resort difficulties
			auto folderIt = m_folders.find(chart->folderId);
			assert(folderIt != m_folders.end());
			folderIt->second->charts.Add(chart);
			m_SortCharts(folderIt->second);

			// Add to search state
			SearchState::ExistingDifficulty ed;
			ed.id = chart->id;
			if (chart->hash.length() == 0)
			{
				ed.lwt = 0;
			}
			else {
				ed.lwt = chart->lwt;

			}
			m_searchState.difficulties.Add(chart->path, ed);
		}

		// Select Scores
		DBStatement scoreScan = m_database.Query("SELECT rowid,score,crit,near,miss,gauge,gameflags,replay,timestamp,chart_hash FROM Scores");
		
		while (scoreScan.StepRow())
		{
			ScoreIndex* score = new ScoreIndex();
			score->id = scoreScan.IntColumn(0);
			score->score = scoreScan.IntColumn(1);
			score->crit = scoreScan.IntColumn(2);
			score->almost = scoreScan.IntColumn(3);
			score->miss = scoreScan.IntColumn(4);
			score->gauge = scoreScan.DoubleColumn(5);
			score->gameflags = scoreScan.IntColumn(6);
			score->replayPath = scoreScan.StringColumn(7);

			score->timestamp = scoreScan.Int64Column(8);
			score->chartHash = scoreScan.StringColumn(9);

			// Add difficulty to map and resort difficulties
			auto diffIt = m_chartsByHash.find(score->chartHash);
			if (diffIt == m_chartsByHash.end()) // If for whatever reason the diff that the score is attatched to is not in the db, ignore the score.
			{
				delete score;
				continue;
			}

			diffIt->second->scores.Add(score);
			m_SortScores(diffIt->second);
		}



		m_outer.OnFoldersCleared.Call(m_folders);
	}
	void m_SortCharts(FolderIndex* folderIndex)
	{
		folderIndex->charts.Sort([](ChartIndex* a, ChartIndex* b)
		{
			if (a->diff_index == b->diff_index)
				return a->level < b->level;
			return a->diff_index < b->diff_index;
		});
	}

	void m_SortScores(ChartIndex* diffIndex)
	{
		diffIndex->scores.Sort([](ScoreIndex* a, ScoreIndex* b)
		{
			return a->score > b->score;
		});
	}

	// Main search thread
	void m_SearchThread()
	{
		Map<String, FileInfo> fileList;

		{
			ProfilerScope $("Chart Database - Enumerate Files and Folders");
			m_outer.OnSearchStatusUpdated.Call("[START] Chart Database - Enumerate Files and Folders");
			for(String rootSearchPath : m_searchPaths)
			{
				Vector<FileInfo> files = Files::ScanFilesRecursive(rootSearchPath, "ksh", &m_interruptSearch);
				if(m_interruptSearch)
					return;
				for(FileInfo& fi : files)
				{
					fileList.Add(fi.fullPath, fi);
				}
			}
			m_outer.OnSearchStatusUpdated.Call("[END] Chart Database - Enumerate Files and Folders");
		}

		{
			ProfilerScope $("Chart Database - Process Removed Files");
			m_outer.OnSearchStatusUpdated.Call("[START] Chart Database - Process Removed Files");
			// Process scanned files
			for(auto f : m_searchState.difficulties)
			{
				if(!fileList.Contains(f.first))
				{
					Event evt;
					evt.action = Event::Removed;
					evt.path = f.first;
					evt.id = f.second.id;
					AddChange(evt);
				}
			}
			m_outer.OnSearchStatusUpdated.Call("[END] Chart Database - Process Removed Files");
		}

		{
			ProfilerScope $("Chart Database - Process New Files");
			m_outer.OnSearchStatusUpdated.Call("[START] Chart Database - Process New Files");
			// Process scanned files
			for(auto f : fileList)
			{
				if(!m_searching)
					break;

				uint64 mylwt = f.second.lastWriteTime;
				Event evt;
				evt.lwt = mylwt;

				SearchState::ExistingDifficulty* existing = m_searchState.difficulties.Find(f.first);
				if(existing)
				{
					evt.id = existing->id;
					if(existing->lwt != mylwt)
					{
						// Map Updated
						evt.action = Event::Updated;
					}
					else
					{
						// Skip, not changed
						continue;
					}
				}
				else
				{
					// Map added
					evt.action = Event::Added;
				}

				Logf("Discovered Chart [%s]", Logger::Info, f.first);
				m_outer.OnSearchStatusUpdated.Call(Utility::Sprintf("Discovered Chart [%s]", f.first));
				// Try to read map metadata
				bool mapValid = false;
				File fileStream;
				Beatmap map;
				if(fileStream.OpenRead(f.first))
				{
					FileReader reader(fileStream);

					if(map.Load(reader, true))
					{
						mapValid = true;
					}
				}

				if(mapValid)
				{
					fileStream.Seek(0);
					evt.mapData = new BeatmapSettings(map.GetMapSettings());

					ProfilerScope $("Chart Database - Hash Chart");
					char data_buffer[0x80];
					uint32_t digest[5];
					sha1::SHA1 s;

					size_t amount_read = 0;
					size_t read_size;
					do
					{
						read_size = fileStream.Read(data_buffer, sizeof(data_buffer));
						amount_read += read_size;
						s.processBytes(data_buffer, read_size);
					}
					while (read_size != 0);
						
					s.getDigest(digest);

					evt.hash = Utility::Sprintf("%08x%08x%08x%08x%08x", digest[0], digest[1], digest[2], digest[3], digest[4]);
				}

				if (!mapValid)
				{
					if(!existing) // Never added
					{
						Logf("Skipping corrupted chart [%s]", Logger::Warning, f.first);
						m_outer.OnSearchStatusUpdated.Call(Utility::Sprintf("Skipping corrupted chart [%s]", f.first));
						if(evt.mapData)
							delete evt.mapData;
						continue;
					}
					// Invalid maps get removed from the database
					evt.action = Event::Removed;
				}
				evt.path = f.first;
				AddChange(evt);
				continue;
			}
			m_outer.OnSearchStatusUpdated.Call("[END] Chart Database - Process New Files");
		}
		m_outer.OnSearchStatusUpdated.Call("");
		m_searching = false;
	}
};

void MapDatabase::FinishInit()
{
	assert(!m_impl);
	m_impl = new MapDatabase_Impl(*this);
}
MapDatabase::MapDatabase(bool postponeInit)
{
	if (!postponeInit)
		FinishInit();
	else
		m_impl = NULL;
}
MapDatabase::MapDatabase()
{
	m_impl = new MapDatabase_Impl(*this);
}
MapDatabase::~MapDatabase()
{
	delete m_impl;
}
void MapDatabase::Update()
{
	m_impl->Update();
}
bool MapDatabase::IsSearching() const
{
	return m_impl->m_searching;
}
void MapDatabase::StartSearching()
{
	m_impl->StartSearching();
}
void MapDatabase::StopSearching()
{
	m_impl->StopSearching();
}
Map<int32, FolderIndex*> MapDatabase::FindFoldersByPath(const String& search)
{
	return m_impl->FindFoldersByPath(search);
}
Map<int32, FolderIndex*> MapDatabase::FindFolders(const String& search)
{
	return m_impl->FindFolders(search);
}
Map<int32, FolderIndex*> MapDatabase::FindFoldersByHash(const String& hash)
{
	return m_impl->FindFoldersByHash(hash);
}
Map<int32, FolderIndex*> MapDatabase::FindFoldersByFolder(const String & folder)
{
	return m_impl->FindFoldersByFolder(folder);
}
Map<int32, FolderIndex*> MapDatabase::FindFoldersByCollection(const String& category)
{
	return m_impl->FindFoldersByCollection(category);
}
FolderIndex* MapDatabase::GetFolder(int32 idx)
{
	FolderIndex** folderIdx = m_impl->m_folders.Find(idx);
	return folderIdx ? *folderIdx : nullptr;
}
Vector<String> MapDatabase::GetCollections()
{
	return m_impl->GetCollections();
}
Vector<String> MapDatabase::GetCollectionsForMap(int32 mapid)
{
	return m_impl->GetCollectionsForMap(mapid);
}
void MapDatabase::AddOrRemoveToCollection(const String& name, int32 mapid)
{
	m_impl->AddOrRemoveToCollection(name, mapid);
}
void MapDatabase::AddSearchPath(const String& path)
{
	m_impl->AddSearchPath(path);
}
void MapDatabase::RemoveSearchPath(const String& path)
{
	m_impl->RemoveSearchPath(path);
}
void MapDatabase::AddScore(const ChartIndex& diff, int score, int crit, int almost, int miss, float gauge, uint32 gameflags, Vector<SimpleHitStat> simpleHitStats, uint64 timestamp)
{
	m_impl->AddScore(diff, score, crit, almost, miss, gauge, gameflags, simpleHitStats, timestamp);
}
ChartIndex* MapDatabase::GetRandomChart()
{
	return m_impl->GetRandomChart();
}