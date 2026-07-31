R"CUAC_INDEX({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "urn:cuac:fixture-index-v1",
  "title": "cuac/v1 offline fixture index",
  "description": "Applied after the RFC 0013 YAML failsafe lexical pass; cross-references, typed values, coverage, row keys, and request agreement are validated against the compiled package.",
  "$ref": "#/$defs/index",
  "$defs": {
    "id": {
      "type": "string",
      "pattern": "^[a-z][a-z0-9_]{0,62}$"
    },
    "coverageKey": {
      "type": "string",
      "pattern": "^[a-z][a-z0-9_]{0,254}$"
    },
    "graphqlName": {
      "type": "string",
      "pattern": "^(?!__)[_A-Za-z][_0-9A-Za-z]{0,254}$"
    },
    "boolean": {"enum": ["true", "false"]},
    "positiveInteger": {"type": "string", "maxLength": 20, "pattern": "^[1-9][0-9]*$"},
    "canonicalInteger": {"type": "string", "pattern": "^-?(?:0|[1-9][0-9]*)$"},
    "digest": {"type": "string", "pattern": "^sha256\\.[0-9a-f]{64}$"},
    "relativeBodyPath": {
      "type": "string",
      "maxLength": 255,
      "pattern": "^[a-z][a-z0-9_.-]{0,254}$"
    },
    "host": {
      "type": "string",
      "maxLength": 253,
      "pattern": "^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)(?:\\.(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?))*$"
    },
    "port": {
      "type": "string",
      "pattern": "^(?:[1-9][0-9]{0,3}|[1-5][0-9]{4}|6[0-4][0-9]{3}|65[0-4][0-9]{2}|655[0-2][0-9]|6553[0-5])$"
    },
    "path": {
      "type": "string",
      "maxLength": 2048,
      "pattern": "^/(?:[A-Za-z0-9._~-]+(?:/[A-Za-z0-9._~-]+)*)?$",
      "not": {"pattern": "(?:^|/)\\.{1,2}(?:/|$)"}
    },
    "origin": {
      "type": "object",
      "additionalProperties": false,
      "required": ["scheme", "host", "port"],
      "properties": {
        "scheme": {"const": "https"},
        "host": {"$ref": "#/$defs/host"},
        "port": {"$ref": "#/$defs/port"}
      }
    },
    "nullValue": {
      "type": "object",
      "additionalProperties": false,
      "required": ["kind"],
      "properties": {"kind": {"const": "null"}}
    },
    "typedValue": {
      "type": "object",
      "additionalProperties": false,
      "required": ["kind", "value"],
      "properties": {
        "kind": {"const": "value"},
        "value": {"type": "string", "maxLength": 1048576}
      }
    },
    "scalarCell": {
      "oneOf": [
        {"type": "string", "maxLength": 1048576},
        {"$ref": "#/$defs/nullValue"}
      ]
    },
    "cell": {
      "oneOf": [
        {"$ref": "#/$defs/scalarCell"},
        {
          "type": "array",
          "maxItems": 100000,
          "items": {"$ref": "#/$defs/scalarCell"}
        }
      ]
    },
    "input": {
      "type": "object",
      "additionalProperties": false,
      "required": ["id", "value"],
      "properties": {
        "id": {"$ref": "#/$defs/id"},
        "value": {
          "oneOf": [
            {"$ref": "#/$defs/nullValue"},
            {"$ref": "#/$defs/typedValue"}
          ]
        }
      }
    },
    "predicate": {
      "type": "object",
      "additionalProperties": false,
      "required": ["column", "operator", "literal"],
      "properties": {
        "column": {"$ref": "#/$defs/id"},
        "operator": {"const": "eq"},
        "literal": {
          "type": "object",
          "additionalProperties": false,
          "required": ["type", "value"],
          "properties": {
            "type": {"enum": ["BOOLEAN", "BIGINT", "VARCHAR"]},
            "value": {"type": "string", "maxLength": 1048576}
          }
        }
      }
    },
    "header": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "value"],
      "properties": {
        "name": {"type": "string", "pattern": "^[A-Za-z][A-Za-z0-9-]{0,62}$"},
        "value": {"type": "string", "maxLength": 1024}
      }
    },
    "encodedQueryField": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "encoded_value"],
      "properties": {
        "name": {"type": "string", "pattern": "^[A-Za-z0-9._~-]{1,63}$"},
        "encoded_value": {"type": "string", "maxLength": 1048576}
      }
    },
    "restRequest": {
      "type": "object",
      "additionalProperties": false,
      "required": ["protocol", "method", "origin", "path", "query", "authorization"],
      "properties": {
        "protocol": {"const": "rest"},
        "method": {"const": "GET"},
        "origin": {"$ref": "#/$defs/origin"},
        "path": {"$ref": "#/$defs/path"},
        "query": {
          "type": "array",
          "maxItems": 64,
          "items": {"$ref": "#/$defs/encodedQueryField"}
        },
        "authorization": {"enum": ["none", "bearer_capability"]}
      }
    },
    "graphqlVariable": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "type", "value"],
      "properties": {
        "name": {"$ref": "#/$defs/graphqlName"},
        "type": {"enum": ["INT_NON_NULL", "STRING_NULLABLE"]},
        "value": {
          "oneOf": [
            {"$ref": "#/$defs/nullValue"},
            {"$ref": "#/$defs/typedValue"}
          ]
        }
      }
    },
    "graphqlRequest": {
      "type": "object",
      "additionalProperties": false,
      "required": ["protocol", "method", "endpoint", "document_digest", "variables", "serialized_body_digest", "authorization"],
      "properties": {
        "protocol": {"const": "graphql"},
        "method": {"const": "POST"},
        "endpoint": {
          "type": "object",
          "additionalProperties": false,
          "required": ["origin", "path"],
          "properties": {
            "origin": {"$ref": "#/$defs/origin"},
            "path": {"$ref": "#/$defs/path"}
          }
        },
        "document_digest": {"$ref": "#/$defs/digest"},
        "variables": {
          "type": "array",
          "minItems": 2,
          "maxItems": 2,
          "items": {"$ref": "#/$defs/graphqlVariable"}
        },
        "serialized_body_digest": {"$ref": "#/$defs/digest"},
        "authorization": {"const": "bearer_capability"}
      }
    },
    "baseResponse": {
      "type": "object",
      "additionalProperties": false,
      "required": ["status", "headers", "body_file", "body_digest"],
      "properties": {
        "status": {"type": "string", "pattern": "^[1-5][0-9]{2}$"},
        "headers": {
          "type": "array",
          "maxItems": 32,
          "items": {"$ref": "#/$defs/header"}
        },
        "body_file": {"$ref": "#/$defs/relativeBodyPath"},
        "body_digest": {"$ref": "#/$defs/digest"}
      }
    },
    "proofResponse": {
      "type": "object",
      "additionalProperties": false,
      "required": ["status", "headers", "body_file", "body_digest", "occurrence_ids"],
      "properties": {
        "status": {"type": "string", "pattern": "^[1-5][0-9]{2}$"},
        "headers": {
          "type": "array",
          "maxItems": 32,
          "items": {"$ref": "#/$defs/header"}
        },
        "body_file": {"$ref": "#/$defs/relativeBodyPath"},
        "body_digest": {"$ref": "#/$defs/digest"},
        "occurrence_ids": {
          "type": "array",
          "maxItems": 100000,
          "items": {"$ref": "#/$defs/id"}
        }
      }
    },
    "executionPage": {
      "type": "object",
      "additionalProperties": false,
      "required": ["request", "response"],
      "properties": {
        "request": {
          "oneOf": [
            {"$ref": "#/$defs/restRequest"},
            {"$ref": "#/$defs/graphqlRequest"}
          ]
        },
        "response": {"$ref": "#/$defs/baseResponse"}
      }
    },
    "proofPage": {
      "type": "object",
      "additionalProperties": false,
      "required": ["request", "response"],
      "properties": {
        "request": {"$ref": "#/$defs/restRequest"},
        "response": {"$ref": "#/$defs/proofResponse"}
      }
    },
    "execution": {
      "type": "object",
      "additionalProperties": false,
      "required": ["pages"],
      "properties": {
        "pages": {
          "type": "array",
          "minItems": 1,
          "maxItems": 32,
          "items": {"$ref": "#/$defs/executionPage"}
        }
      }
    },
    "proofRun": {
      "type": "object",
      "additionalProperties": false,
      "required": ["pages"],
      "properties": {
        "pages": {
          "type": "array",
          "minItems": 1,
          "maxItems": 32,
          "items": {"$ref": "#/$defs/proofPage"}
        }
      }
    },
    "proof": {
      "type": "object",
      "additionalProperties": false,
      "required": ["base", "restricted"],
      "properties": {
        "base": {"$ref": "#/$defs/proofRun"},
        "restricted": {"$ref": "#/$defs/proofRun"}
      }
    },
    "preRequestFailure": {
      "type": "object",
      "additionalProperties": false,
      "required": ["checkpoint"],
      "properties": {
        "checkpoint": {"const": "authorization_resolution"}
      }
    },
    "row": {
      "type": "object",
      "minProperties": 1,
      "propertyNames": {"$ref": "#/$defs/id"},
      "additionalProperties": {"$ref": "#/$defs/cell"}
    },
    "explain": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "protocol": {"enum": ["rest", "graphql"]},
        "pagination": {"enum": ["disabled", "link_next", "response_next", "relay_forward"]},
        "conditional_input": {"$ref": "#/$defs/id"},
        "predicate_owner": {"const": "duckdb"},
        "relation": {"$ref": "#/$defs/id"},
        "operation": {"$ref": "#/$defs/id"},
        "auth": {"enum": ["anonymous", "bearer_capability"]}
      }
    },
    "expectedSuccess": {
      "type": "object",
      "additionalProperties": false,
      "required": ["residual_owner", "remote_accuracy", "rows", "explain"],
      "properties": {
        "residual_owner": {"const": "duckdb"},
        "remote_accuracy": {"enum": ["exact", "superset", "unsupported"]},
        "rows": {
          "type": "array",
          "maxItems": 100000,
          "items": {"$ref": "#/$defs/row"}
        },
        "explain": {"$ref": "#/$defs/explain"}
      }
    },
    "expectedFailure": {
      "type": "object",
      "additionalProperties": false,
      "required": ["diagnostic_code"],
      "properties": {
        "diagnostic_code": {
          "enum": [
            "CUAC_UNSUPPORTED_SPEC",
            "CUAC_UNSUPPORTED_DIALECT",
            "CUAC_MALFORMED_YAML",
            "CUAC_UNKNOWN_FIELD",
            "CUAC_MISSING_FIELD",
            "CUAC_DUPLICATE_ID",
            "CUAC_INVALID_REFERENCE",
            "CUAC_INVALID_IDENTIFIER",
            "CUAC_INVALID_TYPE",
            "CUAC_INVALID_EXTRACTOR",
            "CUAC_RESERVED_INPUT",
            "CUAC_UNSUPPORTED_DECLARATION",
            "CUAC_INVALID_SELECTOR",
            "CUAC_INVALID_PREDICATE",
            "CUAC_INVALID_GRAPHQL_PROFILE",
            "CUAC_POLICY_WIDENING",
            "CUAC_RESOURCE_EXHAUSTED",
            "CUAC_PACKAGE_IDENTITY",
            "CUAC_FIXTURE_MISMATCH",
            "CUAC_INCOMPATIBLE_RELOAD",
            "CUAC_PUBLICATION_CONFLICT"
          ]
        }
      }
    },
    "expectedRuntimeFailure": {
      "type": "object",
      "additionalProperties": false,
      "required": ["runtime_error"],
      "properties": {
        "runtime_error": {
          "type": "object",
          "additionalProperties": false,
          "required": ["stage", "field"],
          "properties": {
            "stage": {"enum": ["transport", "http_status", "decode", "schema", "policy", "resource", "internal", "authentication", "authorization", "remote_protocol"]},
            "field": {"type": "string", "pattern": "^[a-z][a-z0-9_.]{0,126}$"}
          }
        }
      }
    },
    "expectedMissingBearerFailure": {
      "type": "object",
      "additionalProperties": false,
      "required": ["runtime_error"],
      "properties": {
        "runtime_error": {
          "type": "object",
          "additionalProperties": false,
          "required": ["stage", "field"],
          "properties": {
            "stage": {"const": "authentication"},
            "field": {"const": "authorization"}
          }
        }
      }
    },
    "case": {
      "type": "object",
      "additionalProperties": false,
      "required": ["id", "relation", "operation", "covers", "inputs", "auth", "expected"],
      "properties": {
        "id": {"$ref": "#/$defs/id"},
        "relation": {"$ref": "#/$defs/id"},
        "operation": {"$ref": "#/$defs/id"},
        "covers": {
          "type": "array",
          "minItems": 1,
          "uniqueItems": true,
          "items": {"$ref": "#/$defs/coverageKey"}
        },
        "inputs": {
          "type": "array",
          "maxItems": 128,
          "items": {"$ref": "#/$defs/input"}
        },
        "predicate": {"$ref": "#/$defs/predicate"},
        "auth": {"enum": ["anonymous", "bearer_present", "bearer_missing"]},
        "execution": {"$ref": "#/$defs/execution"},
        "proof": {"$ref": "#/$defs/proof"},
        "pre_request_failure": {"$ref": "#/$defs/preRequestFailure"},
        "expected": {
          "oneOf": [
            {"$ref": "#/$defs/expectedSuccess"},
            {"$ref": "#/$defs/expectedFailure"},
            {"$ref": "#/$defs/expectedRuntimeFailure"}
          ]
        }
      },
      "oneOf": [
        {
          "properties": {"execution": {}, "auth": {"enum": ["anonymous", "bearer_present"]}},
          "required": ["execution"],
          "not": {"anyOf": [{"properties": {"proof": {}}, "required": ["proof"]}, {"properties": {"pre_request_failure": {}}, "required": ["pre_request_failure"]}]}
        },
        {
          "properties": {"proof": {}, "predicate": {}, "auth": {"enum": ["anonymous", "bearer_present"]}},
          "required": ["proof", "predicate"],
          "not": {"anyOf": [{"properties": {"execution": {}}, "required": ["execution"]}, {"properties": {"pre_request_failure": {}}, "required": ["pre_request_failure"]}]}
        },
        {
          "properties": {
            "pre_request_failure": {},
            "auth": {"const": "bearer_missing"},
            "expected": {"$ref": "#/$defs/expectedMissingBearerFailure"}
          },
          "required": ["pre_request_failure"],
          "not": {"anyOf": [{"properties": {"execution": {}}, "required": ["execution"]}, {"properties": {"proof": {}}, "required": ["proof"]}]}
        }
      ]
    },
    "index": {
      "type": "object",
      "additionalProperties": false,
      "required": ["api_version", "kind", "package_digest", "cases"],
      "properties": {
        "api_version": {"const": "cuac/v1"},
        "kind": {"const": "fixture_index"},
        "package_digest": {"$ref": "#/$defs/digest"},
        "cases": {
          "type": "array",
          "minItems": 1,
          "maxItems": 1024,
          "items": {"$ref": "#/$defs/case"}
        }
      }
    }
  }
}
)CUAC_INDEX"
