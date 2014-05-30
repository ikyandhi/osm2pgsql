/* Implements the mid-layer processing for osm2pgsql
 * using several PostgreSQL tables
 * 
 * This layer stores data read in from the planet.osm file
 * and is then read by the backend processing code to
 * emit the final geometry-enabled output formats
*/

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <stdexcept>

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

#include "osmtypes.hpp"
#include "reprojection.hpp"
#include "output-pgsql.hpp"
#include "options.hpp"
#include "build_geometry.hpp"
#include "middle.hpp"
#include "pgsql.hpp"
#include "expire-tiles.hpp"
#include "wildcmp.hpp"
#include "node-ram-cache.hpp"
#include "taginfo_impl.hpp"
#include "tagtransform.hpp"
#include "buffer.hpp"
#include "util.hpp"

#include <boost/bind.hpp>
#include <iostream>
#include <limits>

#define SRID (reproj->project_getprojinfo()->srs)

/* FIXME: Shouldn't malloc this all to begin with but call realloc()
   as required. The program will most likely segfault if it reads a
   style file with more styles than this */
#define MAX_STYLES 1000

#define NUM_TABLES (output_pgsql_t::t_MAX)

namespace {

/* NOTE: section below for flags genuinely is static and
 * constant, so there's no need to hoist this into a per
 * class variable. It doesn't get modified, so it's safe
 * to share across threads and its lifetime is the whole
 * program.
 */
struct flagsname {
    flagsname(const char *name_, int flag_)
        : name(strdup(name_)), flag(flag_) {
    }
    char *name;
    int flag;
};

static const flagsname tagflags[] = {
    flagsname("polygon", FLAG_POLYGON),
    flagsname("linear",  FLAG_LINEAR),
    flagsname("nocache", FLAG_NOCACHE),
    flagsname("delete",  FLAG_DELETE),
    flagsname("phstore", FLAG_PHSTORE)
};
#define NUM_FLAGS ((signed)(sizeof(tagflags) / sizeof(tagflags[0])))

int read_style_file( const char *filename, export_list *exlist )
{
  FILE *in;
  int lineno = 0;
  int num_read = 0;
  char osmtype[24];
  char tag[64];
  char datatype[24];
  char flags[128];
  int i;
  char *str;
  int fields;
  struct taginfo temp;
  char buffer[1024];
  int enable_way_area = 1;

  in = fopen( filename, "rt" );
  if( !in )
  {
    fprintf( stderr, "Couldn't open style file '%s': %s\n", filename, strerror(errno) );
    util::exit_nicely();
  }
  
  //for each line of the style file
  while( fgets( buffer, sizeof(buffer), in) != NULL )
  {
    lineno++;
    
    //find where a comment starts and terminate the string there
    str = strchr( buffer, '#' );
    if( str )
      *str = '\0';

    //grab the expected fields for this row
    fields = sscanf( buffer, "%23s %63s %23s %127s", osmtype, tag, datatype, flags );
    if( fields <= 0 )  /* Blank line */
      continue;
    if( fields < 3 )
    {
      fprintf( stderr, "Error reading style file line %d (fields=%d)\n", lineno, fields );
      util::exit_nicely();
    }

    //place to keep info about this tag
    temp.name.assign(tag);
    temp.type.assign(datatype);
    temp.flags = 0;
    
    //split the flags column on commas and keep track of which flags you've seen in a bit mask
    for( str = strtok( flags, ",\r\n" ); str; str = strtok(NULL, ",\r\n") )
    {
      for( i=0; i<NUM_FLAGS; i++ )
      {
        if( strcmp( tagflags[i].name, str ) == 0 )
        {
          temp.flags |= tagflags[i].flag;
          break;
        }
      }
      if( i == NUM_FLAGS )
        fprintf( stderr, "Unknown flag '%s' line %d, ignored\n", str, lineno );
    }

    if ((temp.flags != FLAG_DELETE) && 
        ((temp.name.find('?') != std::string::npos) || 
         (temp.name.find('*') != std::string::npos))) {
        fprintf( stderr, "wildcard '%s' in non-delete style entry\n",temp.name.c_str());
        util::exit_nicely();
    }
    
    if ((temp.name == "way_area") && (temp.flags==FLAG_DELETE)) {
        enable_way_area=0;
    }

    /*    printf("%s %s %d %d\n", temp.name, temp.type, temp.polygon, offset ); */
    bool kept = false;
    
    //keep this tag info if it applies to nodes
    if( strstr( osmtype, "node" ) )
    {
        exlist->add(OSMTYPE_NODE, temp);
        kept = true;
    }

    //keep this tag info if it applies to ways
    if( strstr( osmtype, "way" ) )
    {
        exlist->add(OSMTYPE_WAY, temp);
        kept = true;
    }

    //do we really want to completely quit on an unusable line?
    if( !kept )
    {
      fprintf( stderr, "Weird style line %d\n", lineno );
      util::exit_nicely();
    }
    num_read++;
  }


  if (ferror(in)) {
      perror(filename);
      util::exit_nicely();
  }
  if (num_read == 0) {
      fprintf(stderr, "Unable to parse any valid columns from the style file. Aborting.\n");
      util::exit_nicely();
  }
  fclose(in);
  return enable_way_area;
}
}

void output_pgsql_t::cleanup(void)
{
    for (int i=0; i<NUM_TABLES; i++) {
        m_tables[i]->teardown();
    }
}


/* example from: pg_dump -F p -t planet_osm gis
COPY planet_osm (osm_id, name, place, landuse, leisure, "natural", man_made, waterway, highway, railway, amenity, tourism, learning, building, bridge, layer, way) FROM stdin;
17959841        \N      \N      \N      \N      \N      \N      \N      bus_stop        \N      \N      \N      \N      \N      \N    -\N      0101000020E610000030CCA462B6C3D4BF92998C9B38E04940
17401934        The Horn        \N      \N      \N      \N      \N      \N      \N      \N      pub     \N      \N      \N      \N    -\N      0101000020E6100000C12FC937140FD5BFB4D2F4FB0CE04940
...

mine - 01 01000000 48424298424242424242424256427364
psql - 01 01000020 E6100000 30CCA462B6C3D4BF92998C9B38E04940
       01 01000020 E6100000 48424298424242424242424256427364
0x2000_0000 = hasSRID, following 4 bytes = srid, not supported by geos WKBWriter
Workaround - output SRID=4326;<WKB>
*/

int output_pgsql_t::pgsql_out_node(osmid_t id, struct keyval *tags, double node_lat, double node_lon,
                                   buffer &sql)
{

    int filter = m_tagtransform->filter_node_tags(tags, m_export_list);
    int i;
    struct keyval *tag;

    if (filter) return 1;

    expire->from_bbox(node_lon, node_lat, node_lon, node_lat);
    m_tables[t_point]->write_node(id, tags, node_lat, node_lon, sql);

    return 0;
}

/*static int tag_indicates_polygon(enum OsmType type, const char *key)
{
    int i;

    if (!strcmp(key, "area"))
        return 1;

    for (i=0; i < exportListCount[type]; i++) {
        if( strcmp( exportList[type][i].name, key ) == 0 )
            return exportList[type][i].flags & FLAG_POLYGON;
    }

    return 0;
}*/


/*
COPY planet_osm (osm_id, name, place, landuse, leisure, "natural", man_made, waterway, highway, railway, amenity, tourism, learning, bu
ilding, bridge, layer, way) FROM stdin;
198497  Bedford Road    \N      \N      \N      \N      \N      \N      residential     \N      \N      \N      \N      \N      \N    \N       0102000020E610000004000000452BF702B342D5BF1C60E63BF8DF49406B9C4D470037D5BF5471E316F3DF4940DFA815A6EF35D5BF9AE95E27F5DF4940B41EB
E4C1421D5BF24D06053E7DF4940
212696  Oswald Road     \N      \N      \N      \N      \N      \N      minor   \N      \N      \N      \N      \N      \N      \N    0102000020E610000004000000467D923B6C22D5BFA359D93EE4DF4940B3976DA7AD11D5BF84BBB376DBDF4940997FF44D9A06D5BF4223D8B8FEDF49404D158C4AEA04D
5BF5BB39597FCDF4940
*/
int output_pgsql_t::pgsql_out_way(osmid_t id, struct keyval *tags, struct osmNode *nodes, int count, int exists, buffer &sql)
{
    int polygon = 0, roads = 0;
    int i, wkt_size;
    double split_at;
    double area;

    /* If the flag says this object may exist already, delete it first */
    if(exists) {
        pgsql_delete_way_from_output(id);
        // TODO: this now only has an effect when called from the iterate_ways
        // call-back, so we need some alternative way to trigger this within
        // osmdata_t.
        const std::vector<osmid_t> rel_ids = m_mid->relations_using_way(id);
        for (std::vector<osmid_t>::const_iterator itr = rel_ids.begin();
             itr != rel_ids.end(); ++itr) {
            rels_pending_tracker->mark(*itr);
        }
    }

    if (m_tagtransform->filter_way_tags(tags, &polygon, &roads, m_export_list))
        return 0;
    /* Split long ways after around 1 degree or 100km */
    if (m_options->projection->get_proj_id() == PROJ_LATLONG)
        split_at = 1;
    else
        split_at = 100 * 1000;

    wkt_size = builder.get_wkt_split(nodes, count, polygon, split_at);

    for (i=0;i<wkt_size;i++)
    {
        char *wkt = builder.get_wkt(i);

        if (wkt && strlen(wkt)) {
            /* FIXME: there should be a better way to detect polygons */
            if (!strncmp(wkt, "POLYGON", strlen("POLYGON")) || !strncmp(wkt, "MULTIPOLYGON", strlen("MULTIPOLYGON"))) {
                expire->from_nodes_poly(nodes, count, id);
                area = builder.get_area(i);
                if ((area > 0.0) && m_enable_way_area) {
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "%g", area);
                    addItem(tags, "way_area", tmp, 0);
                }
                m_tables[t_poly]->write_way(id, tags, wkt, sql);
            } else {
                expire->from_nodes_line(nodes, count);
                m_tables[t_line]->write_way(id, tags, wkt, sql);
                if (roads)
                    m_tables[t_roads]->write_way(id, tags, wkt, sql);
            }
        }
        free(wkt);
    }
    builder.clear_wkts();
	
    return 0;
}

int output_pgsql_t::pgsql_out_relation(osmid_t id, struct keyval *rel_tags, int member_count, struct osmNode **xnodes, struct keyval *xtags, int *xcount, osmid_t *xid, const char **xrole, buffer &sql)
{
    int i, wkt_size;
    int roads = 0;
    int make_polygon = 0;
    int make_boundary = 0;
    int * members_superseeded;
    double split_at;

    members_superseeded = (int *)calloc(sizeof(int), member_count);

    if (member_count == 0) {
        free(members_superseeded);
        return 0;
    }

    if (m_tagtransform->filter_rel_member_tags(rel_tags, member_count, xtags, xrole, members_superseeded, &make_boundary, &make_polygon, &roads, m_export_list)) {
        free(members_superseeded);
        return 0;
    }
    
    /* Split long linear ways after around 1 degree or 100km (polygons not effected) */
    if (m_options->projection->get_proj_id() == PROJ_LATLONG)
        split_at = 1;
    else
        split_at = 100 * 1000;

    wkt_size = builder.build(id, xnodes, xcount, make_polygon, m_options->enable_multi, split_at);

    if (!wkt_size) {
        free(members_superseeded);
        return 0;
    }

    for (i=0;i<wkt_size;i++) {
        char *wkt = builder.get_wkt(i);

        if (wkt && strlen(wkt)) {
            expire->from_wkt(wkt, -id);
            /* FIXME: there should be a better way to detect polygons */
            if (!strncmp(wkt, "POLYGON", strlen("POLYGON")) || !strncmp(wkt, "MULTIPOLYGON", strlen("MULTIPOLYGON"))) {
                double area = builder.get_area(i);
                if ((area > 0.0) && m_enable_way_area) {
                    char tmp[32];
                    snprintf(tmp, sizeof(tmp), "%g", area);
                    addItem(rel_tags, "way_area", tmp, 0);
                }
                m_tables[t_poly]->write_way(-id, rel_tags, wkt, sql);
            } else {
                m_tables[t_line]->write_way(-id, rel_tags, wkt, sql);
                if (roads)
                    m_tables[t_roads]->write_way(-id, rel_tags, wkt, sql);
            }
        }
        free(wkt);
    }

    builder.clear_wkts();

    /* Tagtransform will have marked those member ways of the relation that
     * have fully been dealt with as part of the multi-polygon entry.
     * Set them in the database as done and delete their entry to not
     * have duplicates */
    if (make_polygon) {
        for (i=0; xcount[i]; i++) {
            if (members_superseeded[i]) {
                ways_done_tracker->mark(xid[i]);
                pgsql_delete_way_from_output(xid[i]);
            }
        }
    }

    free(members_superseeded);

    /* If we are making a boundary then also try adding any relations which form complete rings
       The linear variants will have already been processed above */
    if (make_boundary) {
        wkt_size = builder.build(id, xnodes, xcount, 1, m_options->enable_multi, split_at);
        for (i=0;i<wkt_size;i++)
        {
            char *wkt = builder.get_wkt(i);

            if (strlen(wkt)) {
                expire->from_wkt(wkt, -id);
                /* FIXME: there should be a better way to detect polygons */
                if (!strncmp(wkt, "POLYGON", strlen("POLYGON")) || !strncmp(wkt, "MULTIPOLYGON", strlen("MULTIPOLYGON"))) {
                    double area = builder.get_area(i);
                    if ((area > 0.0) && m_enable_way_area) {
                        char tmp[32];
                        snprintf(tmp, sizeof(tmp), "%g", area);
                        addItem(rel_tags, "way_area", tmp, 0);
                    }
                    m_tables[t_poly]->write_way(-id, rel_tags, wkt, sql);
                }
            }
            free(wkt);
        }
        builder.clear_wkts();
    }

    return 0;
}

int output_pgsql_t::start()
{
    reproj = m_options->projection;
    builder.set_exclude_broken_polygon(m_options->excludepoly);

    m_export_list = new export_list();

    m_enable_way_area = read_style_file( m_options->style, m_export_list );

    try {
        m_tagtransform = new tagtransform(m_options);
    }
    catch(std::runtime_error& e) {
        fprintf(stderr, "%s\n", e.what());
        fprintf(stderr, "Error: Failed to initialise tag processing.\n");
        util::exit_nicely();
    }

    expire.reset(new expire_tiles(m_options));

    ways_pending_tracker.reset(new pgsql_id_tracker(m_options->conninfo, m_options->prefix, "ways_pending", true));
    ways_done_tracker.reset(new pgsql_id_tracker(m_options->conninfo, m_options->prefix, "ways_done", true));
    rels_pending_tracker.reset(new pgsql_id_tracker(m_options->conninfo, m_options->prefix, "rels_pending", true));

    //for each table
    m_tables.reserve(NUM_TABLES);
    for (int i=0; i<NUM_TABLES; i++) {

        //figure out the columns this table needs
        columns_t columns;
        const std::vector<taginfo> &infos = m_export_list->get((i == t_point)?OSMTYPE_NODE:OSMTYPE_WAY);
        columns.push_back(std::pair<std::string, std::string>("osm_id", POSTGRES_OSMID_TYPE));
        for(std::vector<taginfo>::const_iterator info = infos.begin(); info != infos.end(); ++info)
        {
            if( info->flags & FLAG_DELETE )
                continue;
            if( (info->flags & FLAG_PHSTORE ) == FLAG_PHSTORE)
                continue;
            columns.push_back(std::pair<std::string, std::string>(info->name, info->type));
        }

        //figure out what name we are using for this and what type
        std::string name(m_options->prefix ? m_options->prefix : "");
        std::string type;
        switch(i)
        {
            case t_point:
                name += "_point";
                type = "POINT";
                break;
            case t_line:
                name += "_line";
                type = "LINESTRING";
                break;
            case t_poly:
                name += "_polygon";
                type = "GEOMETRY"; // Actually POLGYON & MULTIPOLYGON but no way to limit to just these two
                break;
            case t_roads:
                name += "_roads";
                type = "LINESTRING";
                break;
            default:
                //TODO: error message about coding error
                util::exit_nicely();
        }

        //tremble in awe of this massive constructor! seriously we are trying to avoid passing an
        //options object because we want to make use of the table_t in output_mutli_t which could
        //have a different tablespace/hstores/etc per table
        m_tables.push_back(boost::shared_ptr<table_t>(
            new table_t(
                name.c_str(), type.c_str(), columns, m_options->hstore_columns, SRID, m_options->scale,
                m_options->append, m_options->slim, m_options->droptemp, m_options->enable_hstore,
                m_options->tblsmain_data, m_options->tblsmain_index
            )
        ));

        //TODO: move this to the constructor and allow it to throw
        //setup the table in postgres
        m_tables.back()->setup(m_options->conninfo);
    }

    return 0;
}


//TODO: does this belong with table as well?
void *output_pgsql_t::pgsql_out_stop_one(void *arg)
{
    table_t *table = (table_t *)arg;
    PGconn *sql_conn = table->sql_conn;

    if( table->buflen != 0 )
    {
       fprintf( stderr, "Internal error: Buffer for %s has %d bytes after end copy", table->name.c_str(), table->buflen );
       util::exit_nicely();
    }

    table->pgsql_pause_copy();
    if (!m_options->append)
    {
        time_t start, end;
        time(&start);
        fprintf(stderr, "Sorting data and creating indexes for %s\n", table->name.c_str());
        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "ANALYZE %s;\n", table->name.c_str());
        fprintf(stderr, "Analyzing %s finished\n", table->name.c_str());
        if (m_options->tblsmain_data) {
            pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE TABLE %s_tmp "
                        "TABLESPACE %s AS SELECT * FROM %s ORDER BY way;\n",
                        table->name.c_str(), m_options->tblsmain_data, table->name.c_str());
        } else {
            pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE TABLE %s_tmp AS SELECT * FROM %s ORDER BY way;\n", table->name.c_str(), table->name.c_str());
        }
        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "DROP TABLE %s;\n", table->name.c_str());
        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "ALTER TABLE %s_tmp RENAME TO %s;\n", table->name.c_str(), table->name.c_str());
        fprintf(stderr, "Copying %s to cluster by geometry finished\n", table->name.c_str());
        fprintf(stderr, "Creating geometry index on  %s\n", table->name.c_str());
        if (m_options->tblsmain_index) {
            /* Use fillfactor 100 for un-updatable imports */
            if (m_options->slim && !m_options->droptemp) {
                pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_index ON %s USING GIST (way) TABLESPACE %s;\n", table->name.c_str(), table->name.c_str(), m_options->tblsmain_index);
            } else {
                pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_index ON %s USING GIST (way) WITH (FILLFACTOR=100) TABLESPACE %s;\n", table->name.c_str(), table->name.c_str(), m_options->tblsmain_index);
            }
        } else {
            if (m_options->slim && !m_options->droptemp) {
                pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_index ON %s USING GIST (way);\n", table->name.c_str(), table->name.c_str());
            } else {
                pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_index ON %s USING GIST (way) WITH (FILLFACTOR=100);\n", table->name.c_str(), table->name.c_str());
            }
        }

        /* slim mode needs this to be able to apply diffs */
        if (m_options->slim && !m_options->droptemp)
        {
            fprintf(stderr, "Creating osm_id index on  %s\n", table->name.c_str());
            if (m_options->tblsmain_index) {
                pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_pkey ON %s USING BTREE (osm_id) TABLESPACE %s;\n", table->name.c_str(), table->name.c_str(), m_options->tblsmain_index);
            } else {
                pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_pkey ON %s USING BTREE (osm_id);\n", table->name.c_str(), table->name.c_str());
            }
        }
        /* Create hstore index if selected */
        if (m_options->enable_hstore_index) {
            fprintf(stderr, "Creating hstore indexes on  %s\n", table->name.c_str());
            if (m_options->tblsmain_index) {
                if (HSTORE_NONE != (m_options->enable_hstore)) {
                    if (m_options->slim && !m_options->droptemp) {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_tags_index ON %s USING GIN (tags) TABLESPACE %s;\n", table->name.c_str(), table->name.c_str(), m_options->tblsmain_index);
                    } else {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_tags_index ON %s USING GIN (tags) TABLESPACE %s;\n", table->name.c_str(), table->name.c_str(), m_options->tblsmain_index);
                    }
                }
                for(size_t i = 0; i < m_options->hstore_columns.size(); ++i) {
                    if (m_options->slim && !m_options->droptemp) {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_hstore_%i_index ON %s USING GIN (\"%s\") TABLESPACE %s;\n",
                               table->name.c_str(), int(i),table->name.c_str(), m_options->hstore_columns[i].c_str(), m_options->tblsmain_index);
                    } else {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_hstore_%i_index ON %s USING GIN (\"%s\") TABLESPACE %s;\n",
                               table->name.c_str(), int(i),table->name.c_str(), m_options->hstore_columns[i].c_str(), m_options->tblsmain_index);
                    }
                }
            } else {
                if (HSTORE_NONE != (m_options->enable_hstore)) {
                    if (m_options->slim && !m_options->droptemp) {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_tags_index ON %s USING GIN (tags);\n", table->name.c_str(), table->name.c_str());
                    } else {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_tags_index ON %s USING GIN (tags) ;\n", table->name.c_str(), table->name.c_str());
                    }
                }
                for(size_t i = 0; i < m_options->hstore_columns.size(); ++i) {
                    if (m_options->slim && !m_options->droptemp) {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_hstore_%i_index ON %s USING GIN (\"%s\");\n", table->name.c_str(), int(i), table->name.c_str(), m_options->hstore_columns[i].c_str());
                    } else {
                        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "CREATE INDEX %s_hstore_%i_index ON %s USING GIN (\"%s\");\n", table->name.c_str(), int(i), table->name.c_str(), m_options->hstore_columns[i].c_str());
                    }
                }
            }
        }
        fprintf(stderr, "Creating indexes on  %s finished\n", table->name.c_str());
        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "GRANT SELECT ON %s TO PUBLIC;\n", table->name.c_str());
        pgsql_exec(sql_conn, PGRES_COMMAND_OK, "ANALYZE %s;\n", table->name.c_str());
        time(&end);
        fprintf(stderr, "All indexes on  %s created  in %ds\n", table->name.c_str(), (int)(end - start));
    }
    PQfinish(sql_conn);
    table->sql_conn = NULL;

    fprintf(stderr, "Completed %s\n", table->name.c_str());
    return NULL;
}

namespace {
/* Using pthreads requires us to shoe-horn everything into various void*
 * pointers. Improvement for the future: just use boost::thread. */
struct pthread_thunk {
    output_pgsql_t *obj;
    void *ptr;
};

extern "C" void *pthread_output_pgsql_stop_one(void *arg) {
    pthread_thunk *thunk = static_cast<pthread_thunk *>(arg);
    return thunk->obj->pgsql_out_stop_one(thunk->ptr);
};
} // anonymous namespace

output_pgsql_t::way_cb_func::way_cb_func(output_pgsql_t *ptr)
    : m_ptr(ptr), m_sql(),
      m_next_internal_id(m_ptr->ways_pending_tracker->pop_mark()) {
}

output_pgsql_t::way_cb_func::~way_cb_func() {}

int output_pgsql_t::way_cb_func::operator()(osmid_t id, struct keyval *tags, struct osmNode *nodes, int count, int exists) {
    if (m_next_internal_id < id) {
        run_internal_until(id, exists);
    }

    if (m_next_internal_id == id) {
        m_next_internal_id = m_ptr->ways_pending_tracker->pop_mark();
    }

    if (m_ptr->ways_done_tracker->is_marked(id)) {
        return 0;
    } else {
        return m_ptr->pgsql_out_way(id, tags, nodes, count, exists, m_sql);
    }
}

void output_pgsql_t::way_cb_func::finish(int exists) {
    run_internal_until(std::numeric_limits<osmid_t>::max(), exists);
}

void output_pgsql_t::way_cb_func::run_internal_until(osmid_t id, int exists) {
    struct keyval tags_int;
    struct osmNode *nodes_int;
    int count_int;
    
    while (m_next_internal_id < id) {
        initList(&tags_int);
        if (!m_ptr->m_mid->ways_get(m_next_internal_id, &tags_int, &nodes_int, &count_int)) {
            if (!m_ptr->ways_done_tracker->is_marked(m_next_internal_id)) {
                m_ptr->pgsql_out_way(m_next_internal_id, &tags_int, nodes_int, count_int, exists, m_sql);
            }
            
            free(nodes_int);
        }
        resetList(&tags_int);
        
        m_next_internal_id = m_ptr->ways_pending_tracker->pop_mark();
    }
}

output_pgsql_t::rel_cb_func::rel_cb_func(output_pgsql_t *ptr)
    : m_ptr(ptr), m_sql(),
      m_next_internal_id(m_ptr->rels_pending_tracker->pop_mark()) {
}

output_pgsql_t::rel_cb_func::~rel_cb_func() {}

int output_pgsql_t::rel_cb_func::operator()(osmid_t id, struct member *mems, int member_count, struct keyval *rel_tags, int exists) {
    if (m_next_internal_id < id) {
        run_internal_until(id, exists);
    }

    if (m_next_internal_id == id) {
        m_next_internal_id = m_ptr->rels_pending_tracker->pop_mark();
    }

    return m_ptr->pgsql_process_relation(id, mems, member_count, rel_tags, exists, m_sql);
}

void output_pgsql_t::rel_cb_func::finish(int exists) {
    run_internal_until(std::numeric_limits<osmid_t>::max(), exists);
}

void output_pgsql_t::rel_cb_func::run_internal_until(osmid_t id, int exists) {
    struct keyval tags_int;
    struct member *members_int;
    int count_int;
    
    while (m_next_internal_id < id) {
        initList(&tags_int);
        if (!m_ptr->m_mid->relations_get(m_next_internal_id, &members_int, &count_int, &tags_int)) {
            m_ptr->pgsql_process_relation(m_next_internal_id, members_int, count_int, &tags_int, exists, m_sql);
            
            free(members_int);
        }
        resetList(&tags_int);
        
        m_next_internal_id = m_ptr->rels_pending_tracker->pop_mark();
    }
}

void output_pgsql_t::commit()
{
    for (int i=0; i<NUM_TABLES; i++) {
        m_tables[i]->commit();
    }

    ways_pending_tracker->commit();
    ways_done_tracker->commit();
    rels_pending_tracker->commit();
}

middle_t::way_cb_func *output_pgsql_t::way_callback()
{
    /* To prevent deadlocks in parallel processing, the mid tables need
     * to stay out of a transaction. In this stage output tables are only
     * written to and not read, so they can be processed as several parallel
     * independent transactions
     */
    for (int i=0; i<NUM_TABLES; i++) {
        m_tables[i]->begin();
    }

    /* Processing any remaing to be processed ways */
    way_cb_func *func = new way_cb_func(this);

    return func;
}

middle_t::rel_cb_func *output_pgsql_t::relation_callback()
{
    /* Processing any remaing to be processed relations */
    /* During this stage output tables also need to stay out of
     * extended transactions, as the delete_way_from_output, called
     * from process_relation, can deadlock if using multi-processing.
     */
    rel_cb_func *rel_callback = new rel_cb_func(this);
    return rel_callback;
}

void output_pgsql_t::stop()
{
    int i;
#ifdef HAVE_PTHREAD
    pthread_t threads[NUM_TABLES];
#endif
  
#ifdef HAVE_PTHREAD
    if (m_options->parallel_indexing) {
      pthread_thunk thunks[NUM_TABLES];
      for (i=0; i<NUM_TABLES; i++) {
          thunks[i].obj = this;
          thunks[i].ptr = m_tables[i].get();
      }

      for (i=0; i<NUM_TABLES; i++) {
          int ret = pthread_create(&threads[i], NULL, pthread_output_pgsql_stop_one, &thunks[i]);
          if (ret) {
              fprintf(stderr, "pthread_create() returned an error (%d)", ret);
              util::exit_nicely();
          }
      }
  
      for (i=0; i<NUM_TABLES; i++) {
          int ret = pthread_join(threads[i], NULL);
          if (ret) {
              fprintf(stderr, "pthread_join() returned an error (%d)", ret);
              util::exit_nicely();
          }
      }
    } else {
#endif

    /* No longer need to access middle layer -- release memory */
    for (i=0; i<NUM_TABLES; i++)
        pgsql_out_stop_one(m_tables[i].get());

#ifdef HAVE_PTHREAD
    }
#endif


    cleanup();
    delete m_export_list;

    expire.reset();
}

int output_pgsql_t::node_add(osmid_t id, double lat, double lon, struct keyval *tags)
{
  pgsql_out_node(id, tags, lat, lon, m_sql);

  return 0;
}

int output_pgsql_t::way_add(osmid_t id, osmid_t *nds, int nd_count, struct keyval *tags)
{
  int polygon = 0;
  int roads = 0;


  /* Check whether the way is: (1) Exportable, (2) Maybe a polygon */
  int filter = m_tagtransform->filter_way_tags(tags, &polygon, &roads, m_export_list);

  /* If this isn't a polygon then it can not be part of a multipolygon
     Hence only polygons are "pending" */
  if (!filter && polygon) { ways_pending_tracker->mark(id); }

  if( !polygon && !filter )
  {
    /* Get actual node data and generate output */
    struct osmNode *nodes = (struct osmNode *)malloc( sizeof(struct osmNode) * nd_count );
    int count = m_mid->nodes_get_list( nodes, nds, nd_count );
    pgsql_out_way(id, tags, nodes, count, 0, m_sql);
    free(nodes);
  }
  return 0;
}

/* This is the workhorse of pgsql_add_relation, split out because it is used as the callback for iterate relations */
int output_pgsql_t::pgsql_process_relation(osmid_t id, struct member *members, int member_count, struct keyval *tags, int exists, buffer &sql)
{
    int i, j, count, count2;
    osmid_t *xid2 = (osmid_t *)malloc( (member_count+1) * sizeof(osmid_t) );
  osmid_t *xid;
  const char **xrole = (const char **)malloc( (member_count+1) * sizeof(const char *) );
  int *xcount = (int *)malloc( (member_count+1) * sizeof(int) );
  struct keyval *xtags  = (struct keyval *)malloc( (member_count+1) * sizeof(struct keyval) );
  struct osmNode **xnodes = (struct osmNode **)malloc( (member_count+1) * sizeof(struct osmNode*) );

  /* If the flag says this object may exist already, delete it first */
  if(exists)
      pgsql_delete_relation_from_output(id);

  if (m_tagtransform->filter_rel_tags(tags, m_export_list)) {
      free(xid2);
      free(xrole);
      free(xcount);
      free(xtags);
      free(xnodes);
      return 1;
  }

  count = 0;
  for( i=0; i<member_count; i++ )
  {
  
    /* Need to handle more than just ways... */
    if( members[i].type != OSMTYPE_WAY )
        continue;
    xid2[count] = members[i].id;
    count++;
  }

  count2 = m_mid->ways_get_list(xid2, count, &xid, xtags, xnodes, xcount);

  for (i = 0; i < count2; i++) {
      for (j = i; j < member_count; j++) {
          if (members[j].id == xid[i]) break;
      }
      xrole[i] = members[j].role;
  }
  xnodes[count2] = NULL;
  xcount[count2] = 0;
  xid[count2] = 0;
  xrole[count2] = NULL;

  /* At some point we might want to consider storing the retrieved data in the members, rather than as separate arrays */
  pgsql_out_relation(id, tags, count2, xnodes, xtags, xcount, xid, xrole, sql);

  for( i=0; i<count2; i++ )
  {
    resetList( &(xtags[i]) );
    free( xnodes[i] );
  }

  free(xid2);
  free(xid);
  free(xrole);
  free(xcount);
  free(xtags);
  free(xnodes);
  return 0;
}

int output_pgsql_t::relation_add(osmid_t id, struct member *members, int member_count, struct keyval *tags)
{
  const char *type = getItem(tags, "type");

  /* Must have a type field or we ignore it */
  if (!type)
      return 0;

  /* Only a limited subset of type= is supported, ignore other */
  if ( (strcmp(type, "route") != 0) && (strcmp(type, "multipolygon") != 0) && (strcmp(type, "boundary") != 0))
    return 0;


  return pgsql_process_relation(id, members, member_count, tags, 0, m_sql);
}
#define UNUSED  __attribute__ ((unused))

/* Delete is easy, just remove all traces of this object. We don't need to
 * worry about finding objects that depend on it, since the same diff must
 * contain the change for that also. */
int output_pgsql_t::node_delete(osmid_t osm_id)
{
    if( !m_options->slim )
    {
        fprintf( stderr, "Cannot apply diffs unless in slim mode\n" );
        util::exit_nicely();
    }
    m_tables[t_point]->pgsql_pause_copy();
    if ( expire->from_db(m_tables[t_point]->sql_conn, osm_id) != 0)
        pgsql_exec(m_tables[t_point]->sql_conn, PGRES_COMMAND_OK, "DELETE FROM %s WHERE osm_id = %" PRIdOSMID, m_tables[t_point]->name.c_str(), osm_id );
    
    return 0;
}

/* Seperated out because we use it elsewhere */
int output_pgsql_t::pgsql_delete_way_from_output(osmid_t osm_id)
{
    /* Optimisation: we only need this is slim mode */
    if( !m_options->slim )
        return 0;
    /* in droptemp mode we don't have indices and this takes ages. */
    if (m_options->droptemp)
        return 0;

    m_tables[t_roads]->pgsql_pause_copy();
    m_tables[t_line]->pgsql_pause_copy();
    m_tables[t_poly]->pgsql_pause_copy();

    m_tables[t_roads]->delete_row(osm_id);
    if ( expire->from_db(m_tables[t_line]->sql_conn, osm_id) != 0)
        m_tables[t_line]->delete_row(osm_id);
    if ( expire->from_db(m_tables[t_poly]->sql_conn, osm_id) != 0)
        m_tables[t_poly]->delete_row(osm_id);
    return 0;
}

int output_pgsql_t::way_delete(osmid_t osm_id)
{
    if( !m_options->slim )
    {
        fprintf( stderr, "Cannot apply diffs unless in slim mode\n" );
        util::exit_nicely();
    }
    pgsql_delete_way_from_output(osm_id);
    return 0;
}

/* Relations are identified by using negative IDs */
int output_pgsql_t::pgsql_delete_relation_from_output(osmid_t osm_id)
{
    m_tables[t_roads]->pgsql_pause_copy();
    m_tables[t_line]->pgsql_pause_copy();
    m_tables[t_poly]->pgsql_pause_copy();

    m_tables[t_roads]->delete_row(-osm_id);
    if ( expire->from_db(m_tables[t_line]->sql_conn, -osm_id) != 0)
        m_tables[t_line]->delete_row(-osm_id);
    if ( expire->from_db(m_tables[t_poly]->sql_conn, -osm_id) != 0)
        m_tables[t_poly]->delete_row(-osm_id);
    return 0;
}

int output_pgsql_t::relation_delete(osmid_t osm_id)
{
    if( !m_options->slim )
    {
        fprintf( stderr, "Cannot apply diffs unless in slim mode\n" );
        util::exit_nicely();
    }
    pgsql_delete_relation_from_output(osm_id);
    return 0;
}

/* Modify is slightly trickier. The basic idea is we simply delete the
 * object and create it with the new parameters. Then we need to mark the
 * objects that depend on this one */
int output_pgsql_t::node_modify(osmid_t osm_id, double lat, double lon, struct keyval *tags)
{
    if( !m_options->slim )
    {
        fprintf( stderr, "Cannot apply diffs unless in slim mode\n" );
        util::exit_nicely();
    }
    node_delete(osm_id);
    node_add(osm_id, lat, lon, tags);
    return 0;
}

int output_pgsql_t::way_modify(osmid_t osm_id, osmid_t *nodes, int node_count, struct keyval *tags)
{
    if( !m_options->slim )
    {
        fprintf( stderr, "Cannot apply diffs unless in slim mode\n" );
        util::exit_nicely();
    }
    way_delete(osm_id);
    way_add(osm_id, nodes, node_count, tags);

    return 0;
}

int output_pgsql_t::relation_modify(osmid_t osm_id, struct member *members, int member_count, struct keyval *tags)
{
    if( !m_options->slim )
    {
        fprintf( stderr, "Cannot apply diffs unless in slim mode\n" );
        util::exit_nicely();
    }
    relation_delete(osm_id);
    relation_add(osm_id, members, member_count, tags);
    return 0;
}

output_pgsql_t::output_pgsql_t(middle_query_t* mid_, const options_t* options_)
    : output_t(mid_, options_) {
}

output_pgsql_t::~output_pgsql_t() {
    if(m_tagtransform != NULL)
    	delete m_tagtransform;
}
