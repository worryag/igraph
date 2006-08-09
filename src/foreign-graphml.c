/* -*- mode: C -*-  */
/* 
   IGraph R package.
   Copyright (C) 2006 Gabor Csardi <csardi@rmki.kfki.hu>
   MTA RMKI, Konkoly-Thege Miklos st. 29-33, Budapest 1121, Hungary
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 
   02110-1301 USA

*/

#include "igraph.h"
#include "config.h"

#include <ctype.h>		/* isspace */
#include <string.h>
#include "memory.h"

#ifdef HAVE_LIBXML
#include <libxml/encoding.h>
#include <libxml/parser.h>

/* TODO: proper error handling */

typedef struct igraph_i_graphml_attribute_record_t {
  const char *id;         	/* GraphML id */
  enum { I_GRAPHML_BOOLEAN, I_GRAPHML_INTEGER, I_GRAPHML_LONG,
	 I_GRAPHML_FLOAT, I_GRAPHML_DOUBLE, I_GRAPHML_STRING,
	 I_GRAPHML_UNKNOWN_TYPE } type;	/* GraphML type */
  igraph_i_attribute_record_t record;
} igraph_i_graphml_attribute_record_t;

int igraph_i_libxml2_read_callback(void *instream, char* buffer, int len) {
  int res;  
  res=fread(buffer, 1, len, (FILE*)instream);
  if (res) return res;
  if (feof((FILE*)instream)) return 0;
  return -1;
}

int igraph_i_libxml2_close_callback(void *instream) { return 0; }

struct igraph_i_graphml_parser_state {
  enum { START, INSIDE_GRAPHML, INSIDE_GRAPH, INSIDE_NODE, INSIDE_EDGE,
      INSIDE_KEY, INSIDE_DEFAULT, INSIDE_DATA, FINISH, UNKNOWN, ERROR } st;
  igraph_t *g;
  igraph_trie_t node_trie;
  igraph_vector_t edgelist;
  unsigned int prev_state;
  unsigned int unknown_depth;
  int index;
  igraph_bool_t successful, edges_directed;
  igraph_trie_t v_names;
  igraph_vector_ptr_t v_attrs;
  igraph_trie_t e_names;
  igraph_vector_ptr_t e_attrs;
  xmlChar *data_key;
  igraph_attribute_elemtype_t data_type;
};

void igraph_i_graphml_sax_handler_error(void *state0, const char* msg, ...) {
  struct igraph_i_graphml_parser_state *state=
    (struct igraph_i_graphml_parser_state*)state0;
  state->successful=0;
  state->st=ERROR;
  /* TODO: use the message */
}

xmlEntityPtr igraph_i_graphml_sax_handler_get_entity(void *state0,
						     const xmlChar* name) {
  return xmlGetPredefinedEntity(name);
}

void igraph_i_graphml_handle_unknown_start_tag(struct igraph_i_graphml_parser_state *state) {
  if (state->st != UNKNOWN) {
    state->prev_state=state->st;
    state->st=UNKNOWN;
    state->unknown_depth=1;
  } else state->unknown_depth++;
}

void igraph_i_graphml_destroy_state(struct igraph_i_graphml_parser_state* state) {
  long int i;

  /* this is the easy part */
  igraph_trie_destroy(&state->node_trie);
  igraph_trie_destroy(&state->v_names);
  igraph_trie_destroy(&state->e_names);
  igraph_vector_destroy(&state->edgelist);
  
  for (i=0; i<igraph_vector_ptr_size(&state->v_attrs); i++) {
    igraph_i_graphml_attribute_record_t *rec=VECTOR(state->v_attrs)[i];
    if (rec->record.type==IGRAPH_ATTRIBUTE_NUMERIC) {
      if (rec->record.value != 0) {
	igraph_vector_destroy((igraph_vector_t*)rec->record.value);
	Free(rec->record.value);
      }
    } else if (rec->record.type==IGRAPH_ATTRIBUTE_STRING) {
      if (rec->record.value != 0) {
	igraph_strvector_destroy((igraph_strvector_t*)rec->record.value);
	Free(rec->record.value);
      }
    }
    Free(rec);
  }	 

  for (i=0; i<igraph_vector_ptr_size(&state->e_attrs); i++) {
    igraph_i_graphml_attribute_record_t *rec=VECTOR(state->e_attrs)[i];
    if (rec->record.type==IGRAPH_ATTRIBUTE_NUMERIC) {
      if (rec->record.value != 0) {
	igraph_vector_destroy((igraph_vector_t*)rec->record.value);
	Free(rec->record.value);
      }
    } else if (rec->record.type==IGRAPH_ATTRIBUTE_STRING) {
      if (rec->record.value != 0) {
	igraph_strvector_destroy((igraph_strvector_t*)rec->record.value);
	Free(rec->record.value);
      }
    }
    Free(rec);
  }	   

  IGRAPH_FINALLY_CLEAN(1);
}

void igraph_i_graphml_sax_handler_start_document(void *state0) {
  struct igraph_i_graphml_parser_state *state=
    (struct igraph_i_graphml_parser_state*)state0;
  int ret;
  
  state->st=START;
  state->successful=1;
  state->edges_directed=0;
  state->data_key=0;
  ret=igraph_vector_ptr_init(&state->v_attrs, 0);
  if (ret) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
  }
  IGRAPH_FINALLY(igraph_vector_ptr_destroy, &state->v_attrs);
  ret=igraph_vector_ptr_init(&state->e_attrs, 0);
  if (ret) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
  }
  IGRAPH_FINALLY(igraph_vector_ptr_destroy, &state->e_attrs);
  ret=igraph_vector_init(&state->edgelist, 0);
  if (ret) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
  }
  IGRAPH_FINALLY(igraph_vector_destroy, &state->edgelist);
  ret=igraph_trie_init(&state->node_trie, 0);
  if (ret) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
  }
  IGRAPH_FINALLY(igraph_trie_destroy, &state->node_trie);
  ret=igraph_trie_init(&state->v_names, 0);
  if (ret) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
  }
  IGRAPH_FINALLY(igraph_trie_destroy, &state->v_names);
  ret=igraph_trie_init(&state->e_names, 0);
  if (ret) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
  }
  IGRAPH_FINALLY(igraph_trie_destroy, &state->e_names);
  
  IGRAPH_FINALLY_CLEAN(6);
  IGRAPH_FINALLY(igraph_i_graphml_destroy_state, state);
}

void igraph_i_graphml_sax_handler_end_document(void *state0) {
  struct igraph_i_graphml_parser_state *state=
    (struct igraph_i_graphml_parser_state*)state0;
  long i, l;
  int r;

  if (state->index<0) {

    igraph_vector_ptr_t vattr, eattr;
    r=igraph_vector_ptr_init(&vattr, igraph_vector_ptr_size(&state->v_attrs));
    if (r) {
      igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, r);
    }
    IGRAPH_FINALLY(igraph_vector_ptr_destroy, &vattr);
    r=igraph_vector_ptr_init(&eattr, igraph_vector_ptr_size(&state->e_attrs));
    if (r) {
      igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, r);
    }
    IGRAPH_FINALLY(igraph_vector_ptr_destroy, &eattr);

    for (i=0; i<igraph_vector_ptr_size(&state->v_attrs); i++) {
      igraph_i_graphml_attribute_record_t *graphmlrec=
	VECTOR(state->v_attrs)[i];
      igraph_i_attribute_record_t *rec=&graphmlrec->record;
      if (rec->type == IGRAPH_ATTRIBUTE_NUMERIC) {
	igraph_vector_t *vec=(igraph_vector_t*)rec->value;
	long int origsize=igraph_vector_size(vec);
	long int nodes=igraph_trie_size(&state->node_trie);
	igraph_vector_resize(vec, nodes);
	for (l=origsize; l<nodes; l++) {
	  VECTOR(*vec)[l]=0.0/0.0;
	}
      } else if (rec->type == IGRAPH_ATTRIBUTE_STRING) {
	igraph_strvector_t *strvec=(igraph_strvector_t*)rec->value;
	long int origsize=igraph_strvector_size(strvec);
	long int nodes=igraph_trie_size(&state->node_trie);
	igraph_strvector_resize(strvec, nodes);
	for (l=origsize; l<nodes; l++) {
	  igraph_strvector_set(strvec, l, "");
	}
      }
      VECTOR(vattr)[i]=rec;
    }

    for (i=0; i<igraph_vector_ptr_size(&state->e_attrs); i++) {
      igraph_i_graphml_attribute_record_t *graphmlrec=
	VECTOR(state->e_attrs)[i];
      igraph_i_attribute_record_t *rec=&graphmlrec->record;
      if (rec->type == IGRAPH_ATTRIBUTE_NUMERIC) {
	igraph_vector_t *vec=(igraph_vector_t*)rec->value;
	long int origsize=igraph_vector_size(vec);
	long int edges=igraph_vector_size(&state->edgelist)/2;
	igraph_vector_resize(vec, edges);
	for (l=origsize; l<edges; l++) {
	  VECTOR(*vec)[l]=0.0/0.0;
	}
      } else if (rec->type == IGRAPH_ATTRIBUTE_STRING) {
	igraph_strvector_t *strvec=(igraph_strvector_t*)rec->value;
	long int origsize=igraph_strvector_size(strvec);
	long int edges=igraph_vector_size(&state->edgelist)/2;
	igraph_strvector_resize(strvec, edges);
	for (l=origsize; l<edges; l++) {
	  igraph_strvector_set(strvec, l, "");
	}
      }
      VECTOR(eattr)[i]=rec;
    }
    
    igraph_empty(state->g, 0, state->edges_directed);
    igraph_add_vertices(state->g, igraph_trie_size(&state->node_trie),
			&vattr);
    igraph_add_edges(state->g, &state->edgelist, &eattr);

    igraph_vector_ptr_destroy(&vattr);
    igraph_vector_ptr_destroy(&eattr);
    IGRAPH_FINALLY_CLEAN(2);
  }

  igraph_i_graphml_destroy_state(state);
}

#define toXmlChar(a)   (BAD_CAST(a))
#define fromXmlChar(a) ((char *)(a)) /* not the most elegant way... */

void igraph_i_graphml_add_attribute_key(const xmlChar** attrs, 
					struct igraph_i_graphml_parser_state *state) {
  xmlChar **it;
  igraph_trie_t *trie;
  igraph_vector_ptr_t *ptrvector;
  long int id;
  int ret;
  igraph_i_graphml_attribute_record_t *rec=
    Calloc(1, igraph_i_graphml_attribute_record_t);
  if (rec==0) { 
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, 
		 IGRAPH_ENOMEM);
  }
  IGRAPH_FINALLY(igraph_free, rec);
  for (it=(xmlChar**)attrs; *it; it+=2) {
    if (xmlStrEqual(*it, toXmlChar("id"))) {
      const char *id=(const char*)(*(it+1));
      rec->id=strdup(id);
    } else if (xmlStrEqual(*it, toXmlChar("attr.name"))) {
      const char *name=fromXmlChar(*(it+1));
      rec->record.name=strdup(name);
    } else if (xmlStrEqual(*it, toXmlChar("attr.type"))) {
      if (xmlStrEqual(*(it+1), (xmlChar*)"boolean")) { 
	rec->type==I_GRAPHML_BOOLEAN;
	rec->record.type=IGRAPH_ATTRIBUTE_NUMERIC;	    
      } else if (xmlStrEqual(*(it+1), toXmlChar("string"))) {
	rec->type=I_GRAPHML_STRING;
	rec->record.type=IGRAPH_ATTRIBUTE_STRING;
      } else if (xmlStrEqual(*(it+1), toXmlChar("float"))) { 
	rec->type=I_GRAPHML_FLOAT;
	rec->record.type=IGRAPH_ATTRIBUTE_NUMERIC;
      } else if (xmlStrEqual(*(it+1), toXmlChar("double"))) { 
	rec->type=I_GRAPHML_DOUBLE;
	rec->record.type=IGRAPH_ATTRIBUTE_NUMERIC;
      } else if (xmlStrEqual(*(it+1), toXmlChar("int"))) {
	rec->type=I_GRAPHML_INTEGER;
	rec->record.type=IGRAPH_ATTRIBUTE_NUMERIC;
      } else if (xmlStrEqual(*(it+1), toXmlChar("long"))) {
	rec->type=I_GRAPHML_LONG;
	rec->record.type=IGRAPH_ATTRIBUTE_NUMERIC;
      } else {
	igraph_error("Cannot parse GraphML file, unknown attribute type", 
		     __FILE__, __LINE__, IGRAPH_PARSEERROR);
      }
    } else if (xmlStrEqual(*it, toXmlChar("for"))) {
      /* graph, vertex or edge attribute? */
      if (xmlStrEqual(*(it+1), toXmlChar("graph"))) { 
	igraph_error("Cannot parse GraphML file, graph attributes not implemented",
		     __FILE__, __LINE__, IGRAPH_UNIMPLEMENTED);	
      } else if (xmlStrEqual(*(it+1), toXmlChar("node"))) {
	trie=&state->v_names;
	ptrvector=&state->v_attrs;
      } else if (xmlStrEqual(*(it+1), toXmlChar("edge"))) {
	trie=&state->e_names;
	ptrvector=&state->e_attrs;
      } else {
	igraph_error("Cannot parse GraphML file, unknown attribute type",
		     __FILE__, __LINE__, IGRAPH_PARSEERROR);
      }
    }
  }

  /* add to trie, attribues */
  igraph_trie_get(trie, rec->id, &id);
  if (id != igraph_trie_size(trie)-1) {
    igraph_error("Cannot parse GraphML file, duplicate attribute", 
		 __FILE__, __LINE__, IGRAPH_PARSEERROR);
  }
  ret=igraph_vector_ptr_push_back(ptrvector, rec);
  if (ret) {
    igraph_error("Cannot read GraphML file", __FILE__, __LINE__, ret);
  }

  /* create the attribute values */
  switch (rec->record.type) {
    igraph_vector_t *vec;
    igraph_strvector_t *strvec;
  case IGRAPH_ATTRIBUTE_NUMERIC:
    vec=Calloc(1, igraph_vector_t);
    if (vec==0) {
      igraph_error("Cannot parse GraphML file", __FILE__, __LINE__,
		   IGRAPH_ENOMEM);
    }
    rec->record.value=vec;
    igraph_vector_init(vec, 0);    
    break;
  case IGRAPH_ATTRIBUTE_STRING:
    strvec=Calloc(1, igraph_strvector_t);
    if (strvec==0) {
      igraph_error("Cannot parge GraphML file", __FILE__, __LINE__, 
		   IGRAPH_ENOMEM);
    }
    rec->record.value=strvec;
    igraph_strvector_init(strvec, 0);
    break;
  default: break;
  }

  IGRAPH_FINALLY_CLEAN(1);	/* rec */
}

void igraph_i_graphml_attribute_data_setup(struct igraph_i_graphml_parser_state *state,
					   const xmlChar **attrs,
					   igraph_attribute_elemtype_t type) {
  xmlChar **it;
  for (it=(xmlChar**)attrs; *it; it+=2) {
    if (xmlStrEqual(*it, toXmlChar("key"))) {
      if (state->data_key) {
	Free(state->data_key);
      }
      state->data_key=xmlStrdup(*(it+1));
      state->data_type=type;
    } else {
      /* ignore */
    }
  }
}

void igraph_i_graphml_attribute_data_add(struct igraph_i_graphml_parser_state *state,
					 const xmlChar *data) {
  const char *key=fromXmlChar(state->data_key);
  char *chardata;
  const xmlChar *end=xmlStrchr(data, (xmlChar) '<');
  igraph_attribute_elemtype_t type=state->data_type;
  igraph_trie_t *trie;
  igraph_vector_ptr_t *ptrvector;
  igraph_i_graphml_attribute_record_t *graphmlrec;
  igraph_i_attribute_record_t *rec;
  long int recid;
  long int id;
  int ret;

  chardata=Calloc( (end-data)+1, char);  
  if (chardata==0) {
    igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, 
		 IGRAPH_ENOMEM);
  }
  memcpy(chardata, data, (end-data)*sizeof(char));
  chardata[(end-data)]='\0';

  switch (type) {
  case IGRAPH_ATTRIBUTE_VERTEX:
    trie=&state->v_names;
    ptrvector=&state->v_attrs;
    id=igraph_trie_size(&state->node_trie)-1; /* hack */
    break;
  case IGRAPH_ATTRIBUTE_EDGE:
    trie=&state->e_names;
    ptrvector=&state->e_attrs;
    id=igraph_vector_size(&state->edgelist)/2-1; /* hack */
    break;
  defaults:
    /* impossible */
    break;
  }
  
  igraph_trie_get(trie, key, &recid);
  graphmlrec=VECTOR(*ptrvector)[recid];
  rec=&graphmlrec->record;

  switch (rec->type) {
    igraph_vector_t *vec;
    igraph_strvector_t *strvec;
    igraph_real_t num;
    long int s, i;
  case IGRAPH_ATTRIBUTE_NUMERIC:
    vec=(igraph_vector_t *)rec->value;
    s=igraph_vector_size(vec);
    if (id >= s) {
      ret=igraph_vector_resize(vec, id+1);
      if (ret) {
	igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
      }
      for (i=s; i<id; i++) {
	VECTOR(*vec)[i]=0.0/0.0;
      }
    }
    sscanf(chardata, "%lf", &num);
    VECTOR(*vec)[id]=num;
    break;
  case IGRAPH_ATTRIBUTE_STRING:
    strvec=(igraph_strvector_t *)rec->value;
    s=igraph_strvector_size(strvec);
    if (id >= s) {
      ret=igraph_strvector_resize(strvec, id+1);
      if (ret) {
	igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
      }
      for (i=s;i<id;i++) {
	igraph_strvector_set(strvec, i, "");
      }
    }
    ret=igraph_strvector_set(strvec, id, chardata);
    if (ret) {
      igraph_error("Cannot parse GraphML file", __FILE__, __LINE__, ret);
    }
    break;
  default:
    break;
  }

  Free(chardata);
}

void igraph_i_graphml_sax_handler_start_element(void *state0,
						const xmlChar* name,
						const xmlChar** attrs) {
  struct igraph_i_graphml_parser_state *state=
    (struct igraph_i_graphml_parser_state*)state0;
  xmlChar** it;
  long int id1, id2;
  unsigned short int directed;

  switch (state->st) {
  case START:
    /* If we are in the START state and received a graphml tag,
     * change to INSIDE_GRAPHML state. Otherwise, change to UNKNOWN. */
    if (xmlStrEqual(name, toXmlChar("graphml")))
      state->st=INSIDE_GRAPHML;
    else
      igraph_i_graphml_handle_unknown_start_tag(state);
    break;
    
  case INSIDE_GRAPHML:
    /* If we are in the INSIDE_GRAPHML state and received a graph tag,
     * change to INSIDE_GRAPH state if the state->index counter reached
     * zero (this is to handle multiple graphs in the same file).
     * Otherwise, change to UNKNOWN. */
    if (xmlStrEqual(name, toXmlChar("graph"))) {
      if (state->index==0) {
	state->st=INSIDE_GRAPH;
	for (it=(xmlChar**)attrs; *it; it+=2) {
	  if (xmlStrEqual(*it, toXmlChar("edgedefault"))) {
	    if (xmlStrEqual(*(it+1), toXmlChar("directed"))) state->edges_directed=1;
	    else if (xmlStrEqual(*(it+1), toXmlChar("undirected"))) state->edges_directed=0;
	  }
	}
      }
      state->index--;
    } else if (xmlStrEqual(name, toXmlChar("key"))) {
      igraph_i_graphml_add_attribute_key(attrs, state);
      state->st=INSIDE_KEY;
    } else
      igraph_i_graphml_handle_unknown_start_tag(state);
    break;

  case INSIDE_KEY:
    /* If we are in the INSIDE_KEY state, check for default tag */
    if (xmlStrEqual(name, toXmlChar("default"))) state->st=INSIDE_DEFAULT;
    else igraph_i_graphml_handle_unknown_start_tag(state);
    break;

  case INSIDE_DEFAULT:
    /* If we are in the INSIDE_DEFAULT state, every further tag will be unknown */
    igraph_i_graphml_handle_unknown_start_tag(state);
    break;
    
  case INSIDE_GRAPH:
    /* If we are in the INSIDE_GRAPH state, check for node and edge tags */
    if (xmlStrEqual(name, toXmlChar("edge"))) {
      id1=-1; id2=-1; directed=state->edges_directed;
      for (it=(xmlChar**)attrs; *it; it+=2) {
	if (xmlStrEqual(*it, toXmlChar("source"))) {
	  igraph_trie_get(&state->node_trie, fromXmlChar(*(it+1)), &id1);
	}
	if (xmlStrEqual(*it, toXmlChar("target"))) {
	  igraph_trie_get(&state->node_trie, fromXmlChar(*(it+1)), &id2);
	}
      }
      if (id1>=0 && id2>=0) {
	igraph_vector_push_back(&state->edgelist, id1);
	igraph_vector_push_back(&state->edgelist, id2);
      } else {
	igraph_i_graphml_sax_handler_error(state, "Edge with missing source or target encountered");
	return;
      }
      state->st=INSIDE_EDGE;
    } else if (xmlStrEqual(name, toXmlChar("node"))) {
      for (it=(xmlChar**)attrs; *it; it+=2) {
	if (xmlStrEqual(*it, toXmlChar("id"))) {
	  it++;
	  igraph_trie_get(&state->node_trie, fromXmlChar(*it), &id1);
	  break;
	}
      }
      state->st=INSIDE_NODE;
    } else
      igraph_i_graphml_handle_unknown_start_tag(state);
    break;
    
  case INSIDE_NODE:
    if (xmlStrEqual(name, toXmlChar("data"))) {
      igraph_i_graphml_attribute_data_setup(state, attrs,
					    IGRAPH_ATTRIBUTE_VERTEX);
      state->prev_state=state->st;
      state->st=INSIDE_DATA;
    }
    break;
    
  case INSIDE_EDGE:
    if (xmlStrEqual(name, toXmlChar("data"))) {
      igraph_i_graphml_attribute_data_setup(state, attrs, 
					    IGRAPH_ATTRIBUTE_EDGE);
      state->prev_state=state->st;
      state->st=INSIDE_DATA;
    }
    break;
    
  default:
    break;
  }
}

void igraph_i_graphml_sax_handler_end_element(void *state0,
						const xmlChar* name) {
  struct igraph_i_graphml_parser_state *state=
    (struct igraph_i_graphml_parser_state*)state0;
  
  switch (state->st) {
  case INSIDE_GRAPHML:
    state->st=FINISH;
    break;
    
  case INSIDE_GRAPH:
    state->st=INSIDE_GRAPHML;
    break;
    
  case INSIDE_KEY:
    state->st=INSIDE_GRAPHML;
    break;

  case INSIDE_DEFAULT:
    state->st=INSIDE_KEY;
    break;
    
  case INSIDE_NODE:
    state->st=INSIDE_GRAPH;
    break;
    
  case INSIDE_EDGE:
    state->st=INSIDE_GRAPH;
    break;

  case INSIDE_DATA:
    state->st=state->prev_state;
    break;
    
  case UNKNOWN:
    state->unknown_depth--;
    if (!state->unknown_depth) state->st=state->prev_state;
    break;
    
  default:
    break;
  }
}

void igraph_i_graphml_sax_handler_chars(void* state0, const xmlChar* ch, int len) {
  struct igraph_i_graphml_parser_state *state=
    (struct igraph_i_graphml_parser_state*)state0;
  
  switch (state->st) {
  case INSIDE_KEY:
  case INSIDE_DEFAULT:
    break;
    
  case INSIDE_DATA:
    igraph_i_graphml_attribute_data_add(state, ch);
    break;
    
  default:
    // just ignore it
    break;
  }
}

static xmlSAXHandler igraph_i_graphml_sax_handler={
  NULL, NULL, NULL, NULL, NULL,
    igraph_i_graphml_sax_handler_get_entity,
    NULL, NULL, NULL, NULL, NULL, NULL,
    igraph_i_graphml_sax_handler_start_document,
    igraph_i_graphml_sax_handler_end_document,
    igraph_i_graphml_sax_handler_start_element,
    igraph_i_graphml_sax_handler_end_element,
    NULL,
    igraph_i_graphml_sax_handler_chars,
    NULL, NULL, NULL,
    igraph_i_graphml_sax_handler_error,
    igraph_i_graphml_sax_handler_error,
    igraph_i_graphml_sax_handler_error,
};

#endif

/**
 * \ingroup loadsave
 * \function igraph_read_graph_graphml
 * \brief Reads a graph from a GraphML file.
 * 
 * </para><para>
 * GraphML is an XML-based file format for representing various types of
 * graphs. Currently only the most basic import functionality is implemented
 * in igraph: it can read GraphML files without nested graphs and hyperedges.
 * Attributes of the graph are not loaded yet.
 * \param graph Pointer to an uninitialized graph object.
 * \param instream A stream, it should be readable.
 * \param directed Whether the imported graph should be directed. Please
 *              note that if you ask for a directed graph, but the
 *              GraphML file to be read contains an undirected graph,
 *              the resulting \c igraph_t graph will be undirected as
 *              well, but it will appear as directed. If you ask for an
 *              undirected graph, the result will be undirected even if
 *              the original GraphML file contained a directed graph.
 * \param index If the GraphML file contains more than one graph, the one
 *              specified by this index will be loaded. Indices start from
 *              zero, so supply zero here if your GraphML file contains only
 *              a single graph.
 * 
 * \return Error code:
 *         \c IGRAPH_PARSEERROR: if there is a
 *         problem reading the file, or the file is syntactically
 *         incorrect.
 *         \c IGRAPH_UNIMPLEMENTED: the GraphML functionality was disabled
 *         at compile-time
 */
int igraph_read_graph_graphml(igraph_t *graph, FILE *instream,
			      int index) {

#ifdef HAVE_LIBXML
  xmlParserCtxtPtr ctxt;
  struct igraph_i_graphml_parser_state state;
  int res;
  char buffer[4096];

  if (index<0)
    IGRAPH_ERROR("Graph index must be non-negative", IGRAPH_EINVAL);
  
  // Create a progressive parser context
  state.g=graph;
  state.index=index<0?0:index;
  ctxt=xmlCreateIOParserCtxt(&igraph_i_graphml_sax_handler, &state,
			     igraph_i_libxml2_read_callback,
			     igraph_i_libxml2_close_callback,
			     instream, XML_CHAR_ENCODING_NONE);
  if (ctxt==NULL)
    IGRAPH_ERROR("Can't create progressive parser context", IGRAPH_PARSEERROR);

  // Parse the file
  while ((res=fread(buffer, 1, 4096, instream))>0) {
    xmlParseChunk(ctxt, buffer, res, 0);
    if (!state.successful) break;
  }
  xmlParseChunk(ctxt, buffer, res, 1);
  
  // Free the context
  xmlFreeParserCtxt(ctxt);
  if (!state.successful)
    IGRAPH_ERROR("Malformed GraphML file", IGRAPH_PARSEERROR);
  if (state.index>=0)
    IGRAPH_ERROR("Graph index was too large", IGRAPH_EINVAL);
  
  return 0;
#else
  IGRAPH_ERROR("GraphML support is disabled", IGRAPH_UNIMPLEMENTED);
#endif
}

/**
 * \ingroup loadsave
 * \function igraph_write_graph_graphml
 * \brief Writes the graph to a file in GraphML format
 *
 * </para><para>
 * GraphML is an XML-based file format for representing various types of
 * graphs. See the GraphML Primer (http://graphml.graphdrawing.org/primer/graphml-primer.html)
 * for detailed format description.
 * 
 * </para><para>
 * No attributes are written at present.
 *
 * \param graph The graph to write. 
 * \param outstream The stream object to write to, it should be
 *        writable.
 * \return Error code:
 *         \c IGRAPH_EFILE if there is an error
 *         writing the file. 
 *
 * Time complexity: O(|V|+|E|) otherwise. All
 * file operations are expected to have time complexity 
 * O(1). 
 */
int igraph_write_graph_graphml(const igraph_t *graph, FILE *outstream) {
  int ret;
  igraph_integer_t l, vc;
  igraph_eit_t it;
  
  ret=fprintf(outstream, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  ret=fprintf(outstream, "<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\"\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  ret=fprintf(outstream, "         xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  ret=fprintf(outstream, "         xsi:schemaLocation=\"http://graphml.graphdrawing.org/xmlns\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  ret=fprintf(outstream, "         http://graphml.graphdrawing.org/xmlns/1.0/graphml.xsd\">\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  ret=fprintf(outstream, "<!-- Created by igraph -->\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  ret=fprintf(outstream, "  <graph id=\"G\" edgedefault=\"%s\">\n", (igraph_is_directed(graph)?"directed":"undirected"));
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  
  /* Let's dump the nodes first */
  vc=igraph_vcount(graph);
  for (l=0; l<vc; l++) {
    ret=fprintf(outstream, "    <node id=\"n%ld\" />\n", (long)l);
    if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  }
  
  /* Now the edges */
  IGRAPH_CHECK(igraph_eit_create(graph, igraph_ess_all(0), &it));
  IGRAPH_FINALLY(igraph_eit_destroy, &it);
  while (!IGRAPH_EIT_END(it)) {
    igraph_integer_t from, to;
    igraph_edge(graph, IGRAPH_EIT_GET(it), &from, &to);
    ret=fprintf(outstream, "    <edge source=\"n%ld\" target=\"n%ld\" />\n", 
		(long int)from, (long int)to);
    if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
    IGRAPH_EIT_NEXT(it);
  }
  igraph_eit_destroy(&it);
  IGRAPH_FINALLY_CLEAN(1);
  
  ret=fprintf(outstream, "  </graph>\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  fprintf(outstream, "</graphml>\n");
  if (ret<0) IGRAPH_ERROR("Write failed", IGRAPH_EFILE);
  
  return 0;
}

