#include "internal.hpp"

namespace dbdiff::postgresql {
namespace detail {

[[nodiscard]] TablePersistence parse_persistence(const std::string_view value) {
  if (value == "p") {
    return TablePersistence::permanent;
  }
  if (value == "u") {
    return TablePersistence::unlogged;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL table persistence"};
}

[[nodiscard]] IdentityGeneration parse_identity(const std::string_view value) {
  if (value.empty()) {
    return IdentityGeneration::none;
  }
  if (value == "a") {
    return IdentityGeneration::always;
  }
  if (value == "d") {
    return IdentityGeneration::by_default;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL identity generation mode"};
}

[[nodiscard]] GeneratedStorage parse_generated(const std::string_view value) {
  if (value.empty()) {
    return GeneratedStorage::none;
  }
  if (value == "s") {
    return GeneratedStorage::stored;
  }
  if (value == "v") {
    return GeneratedStorage::virtual_column;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL generated-column mode"};
}

[[nodiscard]] ConstraintKind parse_constraint_kind(const std::string_view value,
                                                   const int server_major) {
  if (value == "p") {
    return ConstraintKind::primary_key;
  }
  if (value == "u") {
    return ConstraintKind::unique;
  }
  if (value == "c") {
    return ConstraintKind::check;
  }
  if (value == "f") {
    return ConstraintKind::foreign_key;
  }
  if (value == "n" && server_major >= 18) {
    return ConstraintKind::not_null;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL table constraint kind"};
}

[[nodiscard]] PolicyCommand parse_policy_command(const std::string_view value) {
  if (value == "*") {
    return PolicyCommand::all;
  }
  if (value == "r") {
    return PolicyCommand::select;
  }
  if (value == "a") {
    return PolicyCommand::insert;
  }
  if (value == "w") {
    return PolicyCommand::update;
  }
  if (value == "d") {
    return PolicyCommand::delete_rows;
  }
  throw Error{ErrorCode::unsupported, "unsupported PostgreSQL row-security policy command"};
}

[[nodiscard]] std::vector<std::string>
validate_managed_schemas(const std::vector<std::string>& managed_schemas) {
  std::vector<std::string> schemas = managed_schemas;
  for (const auto& schema : schemas) {
    require_no_nul(schema, "managed schema name");
    if (schema.empty()) {
      throw Error{ErrorCode::configuration, "managed schema name must not be empty"};
    }
    if (schema == metadata_schema) {
      throw Error{ErrorCode::configuration, "the dbdiff metadata schema cannot be managed"};
    }
  }

  std::ranges::sort(schemas);
  if (std::ranges::adjacent_find(schemas) != schemas.end()) {
    throw Error{ErrorCode::configuration, "managed schema names must be unique"};
  }
  return schemas;
}

[[nodiscard]] std::string schema_filter(pqxx::transaction_base& transaction,
                                        const std::vector<std::string>& schemas) {
  std::string filter;
  for (const auto& schema : schemas) {
    if (!filter.empty()) {
      filter += ", ";
    }
    filter += transaction.quote(schema);
  }
  return filter;
}

void append_unsupported_catalog_objects(std::vector<UnsupportedCatalogObject>& objects,
                                        const pqxx::result& rows) {
  for (const auto& row : rows) {
    if (row.size() != 2U || row[0].is_null() || row[1].is_null()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL catalog returned an invalid unsupported-object record"};
    }
    objects.push_back(UnsupportedCatalogObject{
        .kind = row[0].as<std::string>(),
        .identity = row[1].as<std::string>(),
    });
  }
}

[[nodiscard]] std::vector<UnsupportedCatalogObject>
unsupported_catalog_objects(pqxx::transaction_base& transaction, const std::string& filter,
                            const int server_major) {
  std::vector<UnsupportedCatalogObject> objects;
  const std::string qualified =
      "pg_catalog.quote_ident(n.nspname) || '.' || pg_catalog.quote_ident(c.relname)";

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT CASE c.relkind "
          "         WHEN 'p' THEN 'partitioned table' WHEN 'v' THEN 'view' "
          "         WHEN 'm' THEN 'materialized view' WHEN 'S' THEN 'sequence' "
          "         WHEN 'f' THEN 'foreign table' WHEN 'I' THEN 'partitioned index' "
          "         WHEN 'c' THEN 'composite relation' ELSE 'relation kind ' || c.relkind::text "
          "       END, " +
          qualified +
          " FROM pg_catalog.pg_class AS c "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND ("
          "  c.relkind NOT IN ('r', 'i', 'S') OR c.relispartition "
          "  OR (c.relkind = 'r' AND c.relpersistence NOT IN ('p', 'u')) "
          "  OR (c.relkind = 'S' AND NOT EXISTS ("
          "    SELECT 1 FROM pg_catalog.pg_depend AS d "
          "    WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
          "      AND d.objid = c.oid "
          "      AND d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
          "      AND d.deptype = 'i'))"
          ")"));

  append_unsupported_catalog_objects(
      objects, transaction.exec("SELECT 'table inheritance', " + qualified +
                                " FROM pg_catalog.pg_class AS c "
                                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                                "WHERE n.nspname IN (" +
                                filter +
                                ") AND c.relkind = 'r' AND EXISTS ("
                                "  SELECT 1 FROM pg_catalog.pg_inherits AS i "
                                "  WHERE i.inhrelid = c.oid OR i.inhparent = c.oid)"));

  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   "SELECT 'table storage property', " + qualified +
                   " FROM pg_catalog.pg_class AS c "
                   "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                   "LEFT JOIN pg_catalog.pg_am AS am ON am.oid = c.relam "
                   "WHERE n.nspname IN (" +
                   filter +
                   ") AND c.relkind = 'r' AND ("
                   "  c.reloptions IS NOT NULL OR c.reltablespace <> 0 OR c.relreplident <> 'd' "
                   "  OR c.reloftype <> 0 OR am.amname IS DISTINCT FROM 'heap')"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'ownership or privileges', " + qualified +
                       " FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND (c.relacl IS NOT NULL OR c.relowner <> "
                       "  (SELECT r.oid FROM pg_catalog.pg_roles AS r "
                       "   WHERE r.rolname = CURRENT_USER)) "
                       "UNION ALL "
                       "SELECT 'schema ownership or privileges', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_namespace AS n "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND ((n.nspname <> 'public' AND (n.nspacl IS NOT NULL OR n.nspowner <> "
                       "    (SELECT r.oid FROM pg_catalog.pg_roles AS r "
                       "     WHERE r.rolname = CURRENT_USER))) "
                       " OR (n.nspname = 'public' AND NOT ("
                       "    n.nspowner = (SELECT r.oid FROM pg_catalog.pg_roles AS r "
                       "                    WHERE r.rolname = 'pg_database_owner') "
                       "    AND (SELECT pg_catalog.count(*) FROM "
                       "           pg_catalog.aclexplode(n.nspacl)) = 3 "
                       "    AND NOT EXISTS (SELECT 1 FROM pg_catalog.aclexplode(n.nspacl) AS acl "
                       "      WHERE acl.grantor <> n.nspowner OR acl.is_grantable "
                       "         OR NOT ((acl.grantee = n.nspowner "
                       "                  AND acl.privilege_type IN ('CREATE', 'USAGE')) "
                       "             OR (acl.grantee = 0 AND acl.privilege_type = 'USAGE')))))) "
                       "UNION ALL "
                       "SELECT 'default privileges', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_default_acl AS a "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = a.defaclnamespace "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'column storage or privileges', " + qualified +
                       " || '.' || pg_catalog.quote_ident(a.attname) "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped AND ("
                       "  a.attstattarget <> -1 OR a.attstorage <> t.typstorage "
                       "  OR a.attcompression <> ''::pg_catalog.\"char\" OR a.attacl IS NOT NULL "
                       "  OR a.attoptions IS NOT NULL OR a.attfdwoptions IS NOT NULL)"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'external column type', " + qualified +
          " || '.' || pg_catalog.quote_ident(a.attname) || ' -> ' || "
          "       pg_catalog.quote_ident(tn.nspname) || '.' || pg_catalog.quote_ident(t.typname) "
          "FROM pg_catalog.pg_attribute AS a "
          "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid "
          "JOIN pg_catalog.pg_namespace AS tn ON tn.oid = t.typnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
          "  AND tn.nspname <> 'pg_catalog' "
          "UNION ALL "
          "SELECT 'external column collation', " +
          qualified +
          " || '.' || pg_catalog.quote_ident(a.attname) || ' -> ' || "
          "       pg_catalog.quote_ident(cn.nspname) || '.' || "
          "pg_catalog.quote_ident(coll.collname) "
          "FROM pg_catalog.pg_attribute AS a "
          "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "JOIN pg_catalog.pg_collation AS coll ON coll.oid = a.attcollation "
          "JOIN pg_catalog.pg_namespace AS cn ON cn.oid = coll.collnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
          "  AND cn.nspname <> 'pg_catalog'"));

  const std::string expression_sources =
      "WITH sources(kind, identity, classid, objid) AS ("
      " SELECT 'column expression', " +
      qualified +
      " || '.' || pg_catalog.quote_ident(a.attname), "
      "        'pg_catalog.pg_attrdef'::pg_catalog.regclass, ad.oid "
      " FROM pg_catalog.pg_attrdef AS ad "
      " JOIN pg_catalog.pg_attribute AS a "
      "   ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
      " JOIN pg_catalog.pg_class AS c ON c.oid = ad.adrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter +
      ") AND c.relkind = 'r' "
      " UNION ALL "
      " SELECT 'constraint expression', " +
      qualified +
      " || '.' || pg_catalog.quote_ident(con.conname), "
      "        'pg_catalog.pg_constraint'::pg_catalog.regclass, con.oid "
      " FROM pg_catalog.pg_constraint AS con "
      " JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter +
      ") "
      " UNION ALL "
      " SELECT 'index expression', " +
      qualified +
      ", 'pg_catalog.pg_class'::pg_catalog.regclass, c.oid "
      " FROM pg_catalog.pg_index AS i "
      " JOIN pg_catalog.pg_class AS c ON c.oid = i.indexrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter +
      ") "
      " UNION ALL "
      " SELECT 'policy expression', " +
      qualified +
      " || '.' || pg_catalog.quote_ident(p.polname), "
      "        'pg_catalog.pg_policy'::pg_catalog.regclass, p.oid "
      " FROM pg_catalog.pg_policy AS p "
      " JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
      " JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      " WHERE n.nspname IN (" +
      filter + ") ) ";
  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   expression_sources +
                   "SELECT s.kind || ' external dependency', s.identity || ' -> ' || "
                   "       pg_catalog.pg_describe_object(d.refclassid, d.refobjid, d.refobjsubid) "
                   "FROM sources AS s "
                   "JOIN pg_catalog.pg_depend AS d ON d.classid = s.classid AND d.objid = s.objid "
                   "LEFT JOIN pg_catalog.pg_proc AS p ON d.refclassid = "
                   "  'pg_catalog.pg_proc'::pg_catalog.regclass AND p.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS pn ON pn.oid = p.pronamespace "
                   "LEFT JOIN pg_catalog.pg_type AS t ON d.refclassid = "
                   "  'pg_catalog.pg_type'::pg_catalog.regclass AND t.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS tn ON tn.oid = t.typnamespace "
                   "LEFT JOIN pg_catalog.pg_operator AS o ON d.refclassid = "
                   "  'pg_catalog.pg_operator'::pg_catalog.regclass AND o.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS onsp ON onsp.oid = o.oprnamespace "
                   "LEFT JOIN pg_catalog.pg_collation AS coll ON d.refclassid = "
                   "  'pg_catalog.pg_collation'::pg_catalog.regclass AND coll.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS cn ON cn.oid = coll.collnamespace "
                   "LEFT JOIN pg_catalog.pg_opclass AS opc ON d.refclassid = "
                   "  'pg_catalog.pg_opclass'::pg_catalog.regclass AND opc.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS opcn ON opcn.oid = opc.opcnamespace "
                   "LEFT JOIN pg_catalog.pg_opfamily AS opf ON d.refclassid = "
                   "  'pg_catalog.pg_opfamily'::pg_catalog.regclass AND opf.oid = d.refobjid "
                   "LEFT JOIN pg_catalog.pg_namespace AS opfn ON opfn.oid = opf.opfnamespace "
                   "WHERE (pn.oid IS NOT NULL AND pn.nspname <> 'pg_catalog') "
                   "   OR (tn.oid IS NOT NULL AND tn.nspname <> 'pg_catalog') "
                   "   OR (onsp.oid IS NOT NULL AND onsp.nspname <> 'pg_catalog') "
                   "   OR (cn.oid IS NOT NULL AND cn.nspname <> 'pg_catalog') "
                   "   OR (opcn.oid IS NOT NULL AND opcn.nspname <> 'pg_catalog') "
                   "   OR (opfn.oid IS NOT NULL AND opfn.nspname <> 'pg_catalog')"));

  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   "SELECT 'column expression relation dependency', " + qualified +
                   " || '.' || pg_catalog.quote_ident(a.attname) || ' -> ' || "
                   "       pg_catalog.pg_describe_object(d.refclassid, d.refobjid, d.refobjsubid) "
                   "FROM pg_catalog.pg_attrdef AS ad "
                   "JOIN pg_catalog.pg_attribute AS a "
                   "  ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum "
                   "JOIN pg_catalog.pg_class AS c ON c.oid = ad.adrelid "
                   "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                   "JOIN pg_catalog.pg_depend AS d ON d.classid = "
                   "  'pg_catalog.pg_attrdef'::pg_catalog.regclass AND d.objid = ad.oid "
                   "WHERE n.nspname IN (" +
                   filter +
                   ") AND c.relkind = 'r' "
                   "  AND d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                   "  AND d.refobjid <> ad.adrelid"));

  append_unsupported_catalog_objects(
      objects, transaction.exec(
                   "SELECT 'index state or storage property', " + qualified +
                   " FROM pg_catalog.pg_index AS i "
                   "JOIN pg_catalog.pg_class AS c ON c.oid = i.indexrelid "
                   "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                   "JOIN pg_catalog.pg_am AS am ON am.oid = c.relam "
                   "WHERE n.nspname IN (" +
                   filter +
                   ") AND (NOT i.indisvalid OR NOT i.indisready OR NOT i.indislive "
                   "  OR i.indcheckxmin OR i.indisclustered OR i.indisreplident "
                   "  OR i.indisexclusion OR c.reloptions IS NOT NULL OR c.reltablespace <> 0 "
                   "  OR am.amname NOT IN ('btree', 'hash', 'gist', 'spgist', 'gin', 'brin'))"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'identity sequence dependency', " + qualified +
                       " || '.' || pg_catalog.quote_ident(a.attname) "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "LEFT JOIN pg_catalog.pg_depend AS d "
                       "  ON d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND d.refobjid = c.oid AND d.refobjsubid = a.attnum AND d.deptype = 'i' "
                       "LEFT JOIN pg_catalog.pg_class AS seq "
                       "  ON d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND seq.oid = d.objid AND seq.relkind = 'S' "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
                       "  AND a.attidentity <> '' "
                       "GROUP BY n.nspname, c.relname, a.attname "
                       "HAVING pg_catalog.count(seq.oid) <> 1"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'identity sequence properties', " + qualified +
                       " || '.' || pg_catalog.quote_ident(a.attname) "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_depend AS d "
                       "  ON d.refclassid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND d.refobjid = c.oid AND d.refobjsubid = a.attnum AND d.deptype = 'i' "
                       "JOIN pg_catalog.pg_class AS seq "
                       "  ON d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       " AND seq.oid = d.objid AND seq.relkind = 'S' "
                       "JOIN pg_catalog.pg_namespace AS sn ON sn.oid = seq.relnamespace "
                       "JOIN pg_catalog.pg_sequence AS s ON s.seqrelid = seq.oid "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND a.attnum > 0 AND NOT a.attisdropped "
                       "  AND a.attidentity <> '' AND ("
                       "    sn.oid <> n.oid OR seq.relpersistence <> c.relpersistence "
                       "    OR pg_catalog.octet_length(c.relname || '_' || a.attname || '_seq') > "
                       "       pg_catalog.current_setting('max_identifier_length')::integer "
                       "    OR seq.relname <> c.relname || '_' || a.attname || '_seq' "
                       "    OR a.atttypid NOT IN ('pg_catalog.int2'::pg_catalog.regtype, "
                       "                            'pg_catalog.int4'::pg_catalog.regtype, "
                       "                            'pg_catalog.int8'::pg_catalog.regtype) "
                       "    OR s.seqtypid <> a.atttypid OR s.seqstart <> 1 OR s.seqincrement <> 1 "
                       "    OR s.seqmin <> 1 "
                       "    OR s.seqmax <> CASE a.atttypid "
                       "         WHEN 'pg_catalog.int2'::pg_catalog.regtype THEN 32767 "
                       "         WHEN 'pg_catalog.int4'::pg_catalog.regtype THEN 2147483647 "
                       "         ELSE 9223372036854775807 END "
                       "    OR s.seqcache <> 1 OR s.seqcycle OR seq.reloptions IS NOT NULL "
                       "    OR seq.reltablespace <> 0)"));

  append_unsupported_catalog_objects(
      objects, transaction.exec("SELECT 'internal trigger state', " + qualified +
                                " || '.' || pg_catalog.quote_ident(t.tgname) "
                                "FROM pg_catalog.pg_trigger AS t "
                                "JOIN pg_catalog.pg_class AS c ON c.oid = t.tgrelid "
                                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                                "WHERE n.nspname IN (" +
                                filter + ") AND t.tgisinternal AND t.tgenabled <> 'O'"));

  const auto supported_constraints = server_major >= 18 ? std::string{"('p', 'u', 'c', 'f', 'n')"}
                                                        : std::string{"('p', 'u', 'c', 'f')"};
  std::string unsupported_constraint_properties =
      "con.contype NOT IN " + supported_constraints +
      " OR con.contypid <> 0 OR con.conparentid <> 0 OR con.coninhcount <> 0 "
      "OR (con.contype = 'f' AND NOT EXISTS ("
      "  SELECT 1 FROM pg_catalog.pg_class AS rc "
      "  JOIN pg_catalog.pg_namespace AS rn ON rn.oid = rc.relnamespace "
      "  WHERE rc.oid = con.confrelid AND rn.nspname IN (" +
      filter + ") AND rc.relkind = 'r' AND NOT rc.relispartition))";
  if (server_major >= 18) {
    unsupported_constraint_properties += " OR con.conperiod";
  }
  append_unsupported_catalog_objects(
      objects, transaction.exec("SELECT 'constraint kind or dependency', "
                                "       pg_catalog.quote_ident(n.nspname) || '.' || "
                                "       pg_catalog.quote_ident(c.relname) || '.' || "
                                "       pg_catalog.quote_ident(con.conname) "
                                "FROM pg_catalog.pg_constraint AS con "
                                "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                                "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                                "WHERE n.nspname IN (" +
                                filter + ") AND (" + unsupported_constraint_properties + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'constraint backing index identity', "
                       "       pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) || '.' || "
                       "       pg_catalog.quote_ident(con.conname) || ' -> ' || "
                       "       pg_catalog.quote_ident(idx.relname) "
                       "FROM pg_catalog.pg_constraint AS con "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_class AS idx ON idx.oid = con.conindid "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND con.contype IN ('p', 'u') "
                       "  AND (idx.relnamespace <> c.relnamespace OR idx.relname <> con.conname)"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'extension-owned relation', " + qualified +
                       " FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_class AS c ON d.classid = "
                       "  'pg_catalog.pg_class'::pg_catalog.regclass AND c.oid = d.objid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extension-owned schema', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_namespace AS n ON d.classid = "
                       "  'pg_catalog.pg_namespace'::pg_catalog.regclass AND n.oid = d.objid "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extension-owned constraint', "
                       "       pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) || '.' || "
                       "       pg_catalog.quote_ident(con.conname) "
                       "FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_constraint AS con ON d.classid = "
                       "  'pg_catalog.pg_constraint'::pg_catalog.regclass AND con.oid = d.objid "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extension-owned policy', "
                       "       pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) || '.' || "
                       "       pg_catalog.quote_ident(p.polname) "
                       "FROM pg_catalog.pg_depend AS d "
                       "JOIN pg_catalog.pg_policy AS p ON d.classid = "
                       "  'pg_catalog.pg_policy'::pg_catalog.regclass AND p.oid = d.objid "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE d.deptype = 'e' AND n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'type', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(t.typname) "
          "FROM pg_catalog.pg_type AS t "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = t.typnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") AND NOT EXISTS ("
          "  SELECT 1 FROM pg_catalog.pg_class AS c "
          "  WHERE c.reltype = t.oid AND c.relkind = 'r' AND NOT c.relispartition) "
          "AND NOT EXISTS ("
          "  SELECT 1 FROM pg_catalog.pg_type AS row_type "
          "  JOIN pg_catalog.pg_class AS c ON c.reltype = row_type.oid "
          "  WHERE row_type.typarray = t.oid AND c.relkind = 'r' AND NOT c.relispartition)"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'routine', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(p.proname) || '(' || "
                       "       pg_catalog.pg_get_function_identity_arguments(p.oid) || ')' "
                       "FROM pg_catalog.pg_proc AS p "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = p.pronamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'trigger', " +
                       qualified +
                       " || '.' || pg_catalog.quote_ident(t.tgname) "
                       "FROM pg_catalog.pg_trigger AS t "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = t.tgrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE NOT t.tgisinternal AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'rule', " +
                       qualified +
                       " || '.' || pg_catalog.quote_ident(r.rulename) "
                       "FROM pg_catalog.pg_rewrite AS r "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = r.ev_class "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE r.rulename <> '_RETURN' AND n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'extended statistics', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(s.stxname) "
                       "FROM pg_catalog.pg_statistic_ext AS s "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = s.stxnamespace "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'text search configuration', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.cfgname) "
          "FROM pg_catalog.pg_ts_config AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.cfgnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'text search dictionary', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.dictname) "
          "FROM pg_catalog.pg_ts_dict AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.dictnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'text search parser', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.prsname) "
          "FROM pg_catalog.pg_ts_parser AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.prsnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'text search template', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(o.tmplname) "
          "FROM pg_catalog.pg_ts_template AS o "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.tmplnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'publication membership', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) || ' in ' || "
          "       pg_catalog.quote_ident(p.pubname) "
          "FROM pg_catalog.pg_publication_rel AS pr "
          "JOIN pg_catalog.pg_publication AS p ON p.oid = pr.prpubid "
          "JOIN pg_catalog.pg_class AS c ON c.oid = pr.prrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'security label', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(c.relname) "
                       "FROM pg_catalog.pg_seclabel AS s "
                       "JOIN pg_catalog.pg_class AS c ON s.classoid = "
                       "  'pg_catalog.pg_class'::pg_catalog.regclass AND c.oid = s.objoid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'security label', pg_catalog.quote_ident(n.nspname) "
                       "FROM pg_catalog.pg_seclabel AS s "
                       "JOIN pg_catalog.pg_namespace AS n ON s.classoid = "
                       "  'pg_catalog.pg_namespace'::pg_catalog.regclass AND n.oid = s.objoid "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec("SELECT 'collation', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.collname) "
                       "FROM pg_catalog.pg_collation AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.collnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'conversion', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.conname) "
                       "FROM pg_catalog.pg_conversion AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.connamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'operator', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.oprname) "
                       "FROM pg_catalog.pg_operator AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.oprnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'operator class', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.opcname) "
                       "FROM pg_catalog.pg_opclass AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.opcnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "UNION ALL "
                       "SELECT 'operator family', pg_catalog.quote_ident(n.nspname) || '.' || "
                       "       pg_catalog.quote_ident(o.opfname) "
                       "FROM pg_catalog.pg_opfamily AS o "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = o.opfnamespace "
                       "WHERE n.nspname IN (" +
                       filter + ")"));

  append_unsupported_catalog_objects(
      objects,
      transaction.exec(
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) "
          "       || CASE WHEN d.objsubid = 0 THEN '' ELSE '.column#' || d.objsubid::text END "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_class AS c ON d.classoid = "
          "  'pg_catalog.pg_class'::pg_catalog.regclass AND c.oid = d.objoid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_namespace AS n ON d.classoid = "
          "  'pg_catalog.pg_namespace'::pg_catalog.regclass AND n.oid = d.objoid "
          "WHERE n.nspname IN (" +
          filter +
          ") AND NOT (n.nspname = 'public' AND "
          "               d.description = 'standard public schema') "
          "UNION ALL "
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) || '.' || "
          "       pg_catalog.quote_ident(con.conname) "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_constraint AS con ON d.classoid = "
          "  'pg_catalog.pg_constraint'::pg_catalog.regclass AND con.oid = d.objoid "
          "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter +
          ") "
          "UNION ALL "
          "SELECT 'comment', pg_catalog.quote_ident(n.nspname) || '.' || "
          "       pg_catalog.quote_ident(c.relname) || '.' || "
          "       pg_catalog.quote_ident(p.polname) "
          "FROM pg_catalog.pg_description AS d "
          "JOIN pg_catalog.pg_policy AS p ON d.classoid = "
          "  'pg_catalog.pg_policy'::pg_catalog.regclass AND p.oid = d.objoid "
          "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
          "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
          "WHERE n.nspname IN (" +
          filter + ")"));

  return objects;
}

using SnapshotTransaction =
    pqxx::transaction<pqxx::isolation_level::repeatable_read, pqxx::write_policy::read_only>;

[[nodiscard]] SchemaSnapshot
introspect_connection(pqxx::connection& connection,
                      const std::vector<std::string>& requested_schemas) {
  const auto managed_schemas = validate_managed_schemas(requested_schemas);
  SnapshotTransaction transaction{connection};
  execute_no_rows(transaction, "SET LOCAL search_path TO pg_catalog");

  const auto version_row =
      execute_one_row(transaction, "SELECT pg_catalog.current_setting('server_version_num')");
  SchemaSnapshot snapshot{
      .server_version = parse_server_version(version_row[0].as<std::string>()),
      .schemas = {},
      .tables = {},
  };

  if (managed_schemas.empty()) {
    transaction.commit();
    return snapshot;
  }

  const auto filter = schema_filter(transaction, managed_schemas);
  snapshot.unsupported_objects =
      unsupported_catalog_objects(transaction, filter, snapshot.server_version.major);
  const auto schema_rows = transaction.exec("SELECT n.nspname "
                                            "FROM pg_catalog.pg_namespace AS n "
                                            "WHERE n.nspname IN (" +
                                            filter + ")");
  snapshot.schemas.reserve(static_cast<std::size_t>(schema_rows.size()));
  for (const auto& row : schema_rows) {
    snapshot.schemas.push_back(row[0].as<std::string>());
  }

  const auto table_rows =
      transaction.exec("SELECT n.nspname, c.relname, c.relpersistence::text, "
                       "       c.relrowsecurity, c.relforcerowsecurity "
                       "FROM pg_catalog.pg_class AS c "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "  AND c.relkind = 'r' "
                       "  AND NOT c.relispartition "
                       "  AND c.relpersistence IN ('p', 'u') "
                       "  AND NOT EXISTS ("
                       "    SELECT 1 "
                       "    FROM pg_catalog.pg_depend AS d "
                       "    WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       "      AND d.objid = c.oid "
                       "      AND d.deptype = 'e')");

  std::map<QualifiedName, Table> tables;
  for (const auto& row : table_rows) {
    QualifiedName name{row[0].as<std::string>(), row[1].as<std::string>()};
    Table table{
        .name = name,
        .persistence = parse_persistence(row[2].as<std::string>()),
        .row_security = row[3].as<bool>(),
        .force_row_security = row[4].as<bool>(),
        .columns = {},
    };
    if (!tables.emplace(std::move(name), std::move(table)).second) {
      throw Error{ErrorCode::database, "PostgreSQL catalog returned a duplicate table"};
    }
  }

  const auto column_rows =
      transaction.exec("SELECT n.nspname, c.relname, a.attnum, a.attname, "
                       "       pg_catalog.format_type(a.atttypid, a.atttypmod), "
                       "       a.attnotnull, "
                       "       pg_catalog.pg_get_expr(ad.adbin, ad.adrelid, false), "
                       "       a.attidentity::text, a.attgenerated::text, "
                       "       CASE WHEN a.attcollation <> 0 AND a.attcollation <> t.typcollation "
                       "            THEN cn.nspname ELSE NULL END, "
                       "       CASE WHEN a.attcollation <> 0 AND a.attcollation <> t.typcollation "
                       "            THEN coll.collname ELSE NULL END "
                       "FROM pg_catalog.pg_attribute AS a "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "JOIN pg_catalog.pg_type AS t ON t.oid = a.atttypid "
                       "LEFT JOIN pg_catalog.pg_attrdef AS ad "
                       "       ON ad.adrelid = a.attrelid AND ad.adnum = a.attnum "
                       "LEFT JOIN pg_catalog.pg_collation AS coll ON coll.oid = a.attcollation "
                       "LEFT JOIN pg_catalog.pg_namespace AS cn ON cn.oid = coll.collnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") "
                       "  AND c.relkind = 'r' "
                       "  AND NOT c.relispartition "
                       "  AND c.relpersistence IN ('p', 'u') "
                       "  AND a.attnum > 0 "
                       "  AND NOT a.attisdropped "
                       "  AND NOT EXISTS ("
                       "    SELECT 1 "
                       "    FROM pg_catalog.pg_depend AS d "
                       "    WHERE d.classid = 'pg_catalog.pg_class'::pg_catalog.regclass "
                       "      AND d.objid = c.oid "
                       "      AND d.deptype = 'e')");

  for (const auto& row : column_rows) {
    const QualifiedName table_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto table = tables.find(table_name);
    if (table == tables.end()) {
      throw Error{ErrorCode::database, "PostgreSQL catalog returned a column without its table"};
    }

    std::optional<std::string> default_expression;
    if (!row[6].is_null()) {
      default_expression = row[6].as<std::string>();
    }

    std::optional<QualifiedName> collation;
    if (!row[9].is_null() || !row[10].is_null()) {
      if (row[9].is_null() || row[10].is_null()) {
        throw Error{ErrorCode::database, "PostgreSQL returned an incomplete collation name"};
      }
      collation = QualifiedName{row[9].as<std::string>(), row[10].as<std::string>()};
    }

    table->second.columns.push_back(Column{
        .position = row[2].as<int>(),
        .name = row[3].as<std::string>(),
        .type = row[4].as<std::string>(),
        .not_null = row[5].as<bool>(),
        .default_expression = std::move(default_expression),
        .identity = parse_identity(row[7].as<std::string>()),
        .generated = parse_generated(row[8].as<std::string>()),
        .collation = std::move(collation),
    });
  }

  const auto constraint_kinds = snapshot.server_version.major >= 18
                                    ? std::string{"('p', 'u', 'c', 'f', 'n')"}
                                    : std::string{"('p', 'u', 'c', 'f')"};
  const auto enforced_expression =
      snapshot.server_version.major >= 18 ? std::string{"con.conenforced"} : std::string{"TRUE"};
  const auto constraint_rows = transaction.exec(
      "SELECT n.nspname, c.relname, con.conname, con.contype::text, "
      "       pg_catalog.pg_get_constraintdef(con.oid, false), con.convalidated, " +
      enforced_expression +
      ", rn.nspname, rc.relname "
      "FROM pg_catalog.pg_constraint AS con "
      "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      "LEFT JOIN pg_catalog.pg_class AS rc ON rc.oid = con.confrelid "
      "LEFT JOIN pg_catalog.pg_namespace AS rn ON rn.oid = rc.relnamespace "
      "WHERE n.nspname IN (" +
      filter + ") AND c.relkind = 'r' AND NOT c.relispartition AND con.contype IN " +
      constraint_kinds +
      " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
      "          con.conname COLLATE \"C\"");

  using ConstraintIdentity = std::tuple<QualifiedName, std::string>;
  std::map<ConstraintIdentity, std::size_t> constraint_positions;
  snapshot.constraints.reserve(static_cast<std::size_t>(constraint_rows.size()));
  for (const auto& row : constraint_rows) {
    const QualifiedName table_name{row[0].as<std::string>(), row[1].as<std::string>()};
    std::optional<QualifiedName> referenced_table;
    if (!row[7].is_null() || !row[8].is_null()) {
      if (row[7].is_null() || row[8].is_null()) {
        throw Error{ErrorCode::database, "PostgreSQL returned an incomplete referenced-table name"};
      }
      referenced_table = QualifiedName{row[7].as<std::string>(), row[8].as<std::string>()};
    }
    const auto name = row[2].as<std::string>();
    const auto position = snapshot.constraints.size();
    if (!constraint_positions.emplace(ConstraintIdentity{table_name, name}, position).second) {
      throw Error{ErrorCode::database, "PostgreSQL returned a duplicate table constraint"};
    }
    snapshot.constraints.push_back(TableConstraint{
        .table = table_name,
        .name = name,
        .kind = parse_constraint_kind(row[3].as<std::string>(), snapshot.server_version.major),
        .columns = {},
        .definition = row[4].as<std::string>(),
        .validated = row[5].as<bool>(),
        .enforced = row[6].as<bool>(),
        .referenced_table = std::move(referenced_table),
        .referenced_columns = {},
    });
  }

  const auto constraint_column_rows = transaction.exec(
      "SELECT n.nspname, c.relname, con.conname, key.ordinality, a.attname "
      "FROM pg_catalog.pg_constraint AS con "
      "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      "CROSS JOIN LATERAL pg_catalog.unnest(con.conkey) WITH ORDINALITY "
      "  AS key(attnum, ordinality) "
      "JOIN pg_catalog.pg_attribute AS a "
      "  ON a.attrelid = con.conrelid AND a.attnum = key.attnum "
      "WHERE n.nspname IN (" +
      filter + ") AND c.relkind = 'r' AND NOT c.relispartition AND con.contype IN " +
      constraint_kinds +
      " ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
      "          con.conname COLLATE \"C\", key.ordinality");
  for (const auto& row : constraint_column_rows) {
    const ConstraintIdentity identity{
        QualifiedName{row[0].as<std::string>(), row[1].as<std::string>()},
        row[2].as<std::string>()};
    const auto found = constraint_positions.find(identity);
    if (found == constraint_positions.end()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL returned a constraint column without its constraint"};
    }
    auto& columns = snapshot.constraints[found->second].columns;
    if (row[3].as<std::size_t>() != columns.size() + 1U) {
      throw Error{ErrorCode::database, "PostgreSQL constraint columns are not contiguous"};
    }
    columns.push_back(row[4].as<std::string>());
  }

  const auto referenced_column_rows =
      transaction.exec("SELECT n.nspname, c.relname, con.conname, key.ordinality, a.attname "
                       "FROM pg_catalog.pg_constraint AS con "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = con.conrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "CROSS JOIN LATERAL pg_catalog.unnest(con.confkey) WITH ORDINALITY "
                       "  AS key(attnum, ordinality) "
                       "JOIN pg_catalog.pg_attribute AS a "
                       "  ON a.attrelid = con.confrelid AND a.attnum = key.attnum "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND NOT c.relispartition AND con.contype = 'f' "
                       "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "         con.conname COLLATE \"C\", key.ordinality");
  for (const auto& row : referenced_column_rows) {
    const ConstraintIdentity identity{
        QualifiedName{row[0].as<std::string>(), row[1].as<std::string>()},
        row[2].as<std::string>()};
    const auto found = constraint_positions.find(identity);
    if (found == constraint_positions.end()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL returned a referenced column without its constraint"};
    }
    auto& columns = snapshot.constraints[found->second].referenced_columns;
    if (row[3].as<std::size_t>() != columns.size() + 1U) {
      throw Error{ErrorCode::database,
                  "PostgreSQL referenced constraint columns are not contiguous"};
    }
    columns.push_back(row[4].as<std::string>());
  }

  const auto standalone_index_predicate =
      "NOT EXISTS (SELECT 1 FROM pg_catalog.pg_constraint AS con "
      "            WHERE con.conindid = i.indexrelid "
      "              AND con.contype IN ('p', 'u', 'x'))";
  const auto index_rows = transaction.exec(
      "SELECT ni.nspname, ci.relname, nt.nspname, ct.relname, am.amname, "
      "       i.indisunique, i.indnullsnotdistinct, "
      "       pg_catalog.pg_get_expr(i.indpred, i.indrelid, false), "
      "       pg_catalog.pg_get_indexdef(i.indexrelid, 0, false), i.indnkeyatts "
      "FROM pg_catalog.pg_index AS i "
      "JOIN pg_catalog.pg_class AS ci ON ci.oid = i.indexrelid "
      "JOIN pg_catalog.pg_namespace AS ni ON ni.oid = ci.relnamespace "
      "JOIN pg_catalog.pg_class AS ct ON ct.oid = i.indrelid "
      "JOIN pg_catalog.pg_namespace AS nt ON nt.oid = ct.relnamespace "
      "JOIN pg_catalog.pg_am AS am ON am.oid = ci.relam "
      "WHERE nt.nspname IN (" +
      filter + ") AND ct.relkind = 'r' AND NOT ct.relispartition AND ci.relkind = 'i' AND " +
      standalone_index_predicate + " ORDER BY ni.nspname COLLATE \"C\", ci.relname COLLATE \"C\"");

  std::map<QualifiedName, std::size_t> index_positions;
  snapshot.indexes.reserve(static_cast<std::size_t>(index_rows.size()));
  for (const auto& row : index_rows) {
    QualifiedName index_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto position = snapshot.indexes.size();
    if (!index_positions.emplace(index_name, position).second) {
      throw Error{ErrorCode::database, "PostgreSQL returned a duplicate standalone index"};
    }
    std::optional<std::string> predicate;
    if (!row[7].is_null()) {
      predicate = row[7].as<std::string>();
    }
    snapshot.indexes.push_back(Index{
        .name = std::move(index_name),
        .table = QualifiedName{row[2].as<std::string>(), row[3].as<std::string>()},
        .method = row[4].as<std::string>(),
        .unique = row[5].as<bool>(),
        .nulls_not_distinct = row[6].as<bool>(),
        .key_expressions =
            index_key_definitions(row[8].as<std::string>(), row[9].as<std::size_t>()),
        .included_columns = {},
        .predicate = std::move(predicate),
    });
  }

  const auto index_component_rows = transaction.exec(
      "SELECT ni.nspname, ci.relname, key.ordinality, i.indnkeyatts, "
      "       a.attname "
      "FROM pg_catalog.pg_index AS i "
      "JOIN pg_catalog.pg_class AS ci ON ci.oid = i.indexrelid "
      "JOIN pg_catalog.pg_namespace AS ni ON ni.oid = ci.relnamespace "
      "JOIN pg_catalog.pg_class AS ct ON ct.oid = i.indrelid "
      "JOIN pg_catalog.pg_namespace AS nt ON nt.oid = ct.relnamespace "
      "CROSS JOIN LATERAL pg_catalog.unnest(i.indkey) WITH ORDINALITY "
      "  AS key(attnum, ordinality) "
      "LEFT JOIN pg_catalog.pg_attribute AS a "
      "  ON a.attrelid = i.indrelid AND a.attnum = key.attnum "
      "WHERE nt.nspname IN (" +
      filter + ") AND ct.relkind = 'r' AND NOT ct.relispartition AND ci.relkind = 'i' AND " +
      standalone_index_predicate + " AND key.ordinality > i.indnkeyatts " +
      " ORDER BY ni.nspname COLLATE \"C\", ci.relname COLLATE \"C\", key.ordinality");
  for (const auto& row : index_component_rows) {
    const QualifiedName index_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto found = index_positions.find(index_name);
    if (found == index_positions.end()) {
      throw Error{ErrorCode::database, "PostgreSQL returned an index component without its index"};
    }
    auto& index = snapshot.indexes[found->second];
    const auto ordinal = row[2].as<std::size_t>();
    const auto key_count = row[3].as<std::size_t>();
    if (ordinal != key_count + index.included_columns.size() + 1U || row[4].is_null()) {
      throw Error{ErrorCode::database, "PostgreSQL included index columns are invalid"};
    }
    index.included_columns.push_back(row[4].as<std::string>());
  }

  const auto policy_rows =
      transaction.exec("SELECT n.nspname, c.relname, p.polname, p.polcmd::text, p.polpermissive, "
                       "       pg_catalog.pg_get_expr(p.polqual, p.polrelid, false), "
                       "       pg_catalog.pg_get_expr(p.polwithcheck, p.polrelid, false) "
                       "FROM pg_catalog.pg_policy AS p "
                       "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
                       "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
                       "WHERE n.nspname IN (" +
                       filter +
                       ") AND c.relkind = 'r' AND NOT c.relispartition "
                       "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
                       "         p.polname COLLATE \"C\"");
  using PolicyIdentity = std::tuple<QualifiedName, std::string>;
  std::map<PolicyIdentity, std::size_t> policy_positions;
  snapshot.policies.reserve(static_cast<std::size_t>(policy_rows.size()));
  for (const auto& row : policy_rows) {
    const QualifiedName table_name{row[0].as<std::string>(), row[1].as<std::string>()};
    const auto name = row[2].as<std::string>();
    const auto position = snapshot.policies.size();
    if (!policy_positions.emplace(PolicyIdentity{table_name, name}, position).second) {
      throw Error{ErrorCode::database, "PostgreSQL returned a duplicate row-security policy"};
    }
    std::optional<std::string> using_expression;
    if (!row[5].is_null()) {
      using_expression = row[5].as<std::string>();
    }
    std::optional<std::string> check_expression;
    if (!row[6].is_null()) {
      check_expression = row[6].as<std::string>();
    }
    snapshot.policies.push_back(RowSecurityPolicy{
        .table = table_name,
        .name = name,
        .command = parse_policy_command(row[3].as<std::string>()),
        .permissive = row[4].as<bool>(),
        .roles = {},
        .using_expression = std::move(using_expression),
        .check_expression = std::move(check_expression),
    });
  }

  const auto policy_role_rows = transaction.exec(
      "SELECT n.nspname, c.relname, p.polname, role.oid = 0, r.rolname "
      "FROM pg_catalog.pg_policy AS p "
      "JOIN pg_catalog.pg_class AS c ON c.oid = p.polrelid "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
      "CROSS JOIN LATERAL pg_catalog.unnest(p.polroles) AS role(oid) "
      "LEFT JOIN pg_catalog.pg_roles AS r ON r.oid = role.oid "
      "WHERE n.nspname IN (" +
      filter +
      ") AND c.relkind = 'r' AND NOT c.relispartition "
      "ORDER BY n.nspname COLLATE \"C\", c.relname COLLATE \"C\", "
      "         p.polname COLLATE \"C\", role.oid = 0 DESC, r.rolname COLLATE \"C\"");
  for (const auto& row : policy_role_rows) {
    const PolicyIdentity identity{QualifiedName{row[0].as<std::string>(), row[1].as<std::string>()},
                                  row[2].as<std::string>()};
    const auto found = policy_positions.find(identity);
    if (found == policy_positions.end()) {
      throw Error{ErrorCode::database, "PostgreSQL returned a policy role without its policy"};
    }
    const auto public_role = row[3].as<bool>();
    if (!public_role && row[4].is_null()) {
      throw Error{ErrorCode::database, "PostgreSQL row-security policy has an unknown role"};
    }
    snapshot.policies[found->second].roles.push_back(PolicyRole{
        .public_role = public_role,
        .name = public_role ? std::string{} : row[4].as<std::string>(),
    });
  }

  snapshot.tables.reserve(tables.size());
  for (auto& [name, table] : tables) {
    static_cast<void>(name);
    snapshot.tables.push_back(std::move(table));
  }
  transaction.commit();
  return normalize_snapshot(std::move(snapshot));
}

void hash_field(Sha256& hash, const std::string_view name, const std::string_view value) {
  hash.add_length_prefixed(name);
  hash.add_length_prefixed(value);
}

void hash_boolean(Sha256& hash, const std::string_view name, const bool value) {
  hash_field(hash, name, value ? "1" : "0");
}

void hash_optional(Sha256& hash, const std::string_view name,
                   const std::optional<std::string>& value) {
  hash_boolean(hash, "present", value.has_value());
  if (value.has_value()) {
    hash_field(hash, name, *value);
  }
}

[[nodiscard]] std::string semantic_hash_normalized(const SchemaSnapshot& snapshot) {
  Sha256 hash;
  hash.add_length_prefixed("dbdiff.postgresql.schema.v3");
  for (const auto& schema : snapshot.schemas) {
    hash_field(hash, "object", "schema");
    hash_field(hash, "name", schema);
  }
  for (const auto& table : snapshot.tables) {
    hash_field(hash, "object", "table");
    hash_field(hash, "schema", table.name.schema);
    hash_field(hash, "name", table.name.name);
    hash_field(hash, "persistence", std::to_string(static_cast<int>(table.persistence)));
    hash_boolean(hash, "row_security", table.row_security);
    hash_boolean(hash, "force_row_security", table.force_row_security);
    for (const auto& column : table.columns) {
      hash_field(hash, "child", "column");
      hash_field(hash, "position", std::to_string(column.position));
      hash_field(hash, "name", column.name);
      hash_field(hash, "type", column.type);
      hash_boolean(hash, "not_null", column.not_null);
      hash_optional(hash, "default", column.default_expression);
      hash_field(hash, "identity", std::to_string(static_cast<int>(column.identity)));
      hash_field(hash, "generated", std::to_string(static_cast<int>(column.generated)));
      hash_boolean(hash, "collation_present", column.collation.has_value());
      if (column.collation.has_value()) {
        hash_field(hash, "collation_schema", column.collation->schema);
        hash_field(hash, "collation_name", column.collation->name);
      }
    }
  }
  for (const auto& constraint : snapshot.constraints) {
    hash_field(hash, "object", "constraint");
    hash_field(hash, "table_schema", constraint.table.schema);
    hash_field(hash, "table_name", constraint.table.name);
    hash_field(hash, "name", constraint.name);
    hash_field(hash, "kind", std::to_string(static_cast<int>(constraint.kind)));
    for (const auto& column : constraint.columns) {
      hash_field(hash, "column", column);
    }
    hash_field(hash, "definition", constraint.definition);
    hash_boolean(hash, "validated", constraint.validated);
    hash_boolean(hash, "enforced", constraint.enforced);
    hash_boolean(hash, "referenced_table_present", constraint.referenced_table.has_value());
    if (const auto referenced_table = constraint.referenced_table; referenced_table.has_value()) {
      const auto& referenced = referenced_table.value();
      hash_field(hash, "referenced_schema", referenced.schema);
      hash_field(hash, "referenced_table", referenced.name);
    }
    for (const auto& column : constraint.referenced_columns) {
      hash_field(hash, "referenced_column", column);
    }
  }
  for (const auto& index : snapshot.indexes) {
    hash_field(hash, "object", "index");
    hash_field(hash, "schema", index.name.schema);
    hash_field(hash, "name", index.name.name);
    hash_field(hash, "table_schema", index.table.schema);
    hash_field(hash, "table_name", index.table.name);
    hash_field(hash, "method", index.method);
    hash_boolean(hash, "unique", index.unique);
    hash_boolean(hash, "nulls_not_distinct", index.nulls_not_distinct);
    for (const auto& expression : index.key_expressions) {
      hash_field(hash, "key_expression", expression);
    }
    for (const auto& column : index.included_columns) {
      hash_field(hash, "included_column", column);
    }
    hash_optional(hash, "predicate", index.predicate);
  }
  for (const auto& policy : snapshot.policies) {
    hash_field(hash, "object", "policy");
    hash_field(hash, "table_schema", policy.table.schema);
    hash_field(hash, "table_name", policy.table.name);
    hash_field(hash, "name", policy.name);
    hash_field(hash, "command", std::to_string(static_cast<int>(policy.command)));
    hash_boolean(hash, "permissive", policy.permissive);
    for (const auto& role : policy.roles) {
      hash_boolean(hash, "public_role", role.public_role);
      hash_field(hash, "role", role.name);
    }
    hash_optional(hash, "using", policy.using_expression);
    hash_optional(hash, "check", policy.check_expression);
  }
  return hash.finish_hex();
}

} // namespace detail

using namespace detail;

SchemaSnapshot normalize_snapshot(SchemaSnapshot snapshot) {
  snapshot.server_version = validate_server_version(snapshot.server_version.number);
  std::ranges::sort(snapshot.schemas);
  if (std::ranges::adjacent_find(snapshot.schemas) != snapshot.schemas.end()) {
    throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains duplicate schemas"};
  }
  for (const auto& schema : snapshot.schemas) {
    require_no_nul(schema, "PostgreSQL schema name");
    if (schema.empty() || schema == metadata_schema) {
      throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains an invalid schema"};
    }
  }

  std::ranges::sort(snapshot.tables, {}, &Table::name);
  if (std::ranges::adjacent_find(snapshot.tables, {}, &Table::name) != snapshot.tables.end()) {
    throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains duplicate tables"};
  }

  for (auto& table : snapshot.tables) {
    if (!std::ranges::binary_search(snapshot.schemas, table.name.schema)) {
      throw Error{ErrorCode::database, "PostgreSQL table belongs to an unknown schema"};
    }
    require_no_nul(table.name.name, "PostgreSQL table name");
    if (table.name.name.empty()) {
      throw Error{ErrorCode::database, "PostgreSQL table name must not be empty"};
    }

    std::ranges::sort(table.columns, [](const Column& left, const Column& right) {
      if (left.position != right.position) {
        return left.position < right.position;
      }
      return left.name < right.name;
    });

    std::set<std::string> column_names;
    int previous_position = 0;
    int canonical_position = 0;
    for (auto& column : table.columns) {
      require_no_nul(column.name, "PostgreSQL column name");
      require_no_nul(column.type, "PostgreSQL column type");
      if (column.default_expression.has_value()) {
        require_no_nul(*column.default_expression, "PostgreSQL column expression");
      }
      if (column.collation.has_value()) {
        require_no_nul(column.collation->schema, "PostgreSQL collation schema");
        require_no_nul(column.collation->name, "PostgreSQL collation name");
        if (column.collation->schema.empty() || column.collation->name.empty()) {
          throw Error{ErrorCode::database,
                      "PostgreSQL schema snapshot contains an invalid collation"};
        }
      }
      if (column.position <= 0 || column.position == previous_position || column.name.empty() ||
          column.type.empty() || !column_names.insert(column.name).second) {
        throw Error{ErrorCode::database, "PostgreSQL schema snapshot contains an invalid column"};
      }
      if (column.identity != IdentityGeneration::none &&
          column.generated != GeneratedStorage::none) {
        throw Error{ErrorCode::database, "PostgreSQL column cannot be both identity and generated"};
      }
      if (column.identity != IdentityGeneration::none && column.default_expression.has_value()) {
        throw Error{ErrorCode::database,
                    "PostgreSQL identity column cannot have a separate default expression"};
      }
      if (column.generated != GeneratedStorage::none && !column.default_expression.has_value()) {
        throw Error{ErrorCode::database, "PostgreSQL generated column is missing its expression"};
      }
      if (column.generated == GeneratedStorage::virtual_column &&
          snapshot.server_version.major < 18) {
        throw Error{ErrorCode::unsupported,
                    "virtual generated columns require PostgreSQL 18 or newer"};
      }
      previous_position = column.position;
      column.position = ++canonical_position;
    }
  }

  std::map<QualifiedName, const Table*> tables;
  for (const auto& table : snapshot.tables) {
    tables.emplace(table.name, &table);
  }
  const auto find_column = [&tables](const QualifiedName& table_name,
                                     const std::string_view column_name) -> const Column* {
    const auto table = tables.find(table_name);
    if (table == tables.end()) {
      return nullptr;
    }
    const auto column = std::ranges::find(table->second->columns, column_name, &Column::name);
    return column == table->second->columns.end() ? nullptr : &*column;
  };

  std::ranges::sort(snapshot.constraints,
                    [](const TableConstraint& left, const TableConstraint& right) {
                      return std::tie(left.table, left.name) < std::tie(right.table, right.name);
                    });
  for (std::size_t index = 0U; index < snapshot.constraints.size(); ++index) {
    auto& constraint = snapshot.constraints[index];
    if (index != 0U && constraint.table == snapshot.constraints[index - 1U].table &&
        constraint.name == snapshot.constraints[index - 1U].name) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains duplicate table constraints"};
    }
    require_no_nul(constraint.name, "PostgreSQL constraint name");
    require_no_nul(constraint.definition, "PostgreSQL constraint definition");
    if (!tables.contains(constraint.table) || constraint.name.empty() ||
        constraint.definition.empty()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains an invalid table constraint"};
    }
    if (constraint.kind == ConstraintKind::not_null && snapshot.server_version.major < 18) {
      throw Error{ErrorCode::unsupported,
                  "named PostgreSQL NOT NULL constraints require PostgreSQL 18 or newer"};
    }

    std::set<std::string, std::less<>> columns;
    for (const auto& column : constraint.columns) {
      require_no_nul(column, "PostgreSQL constraint column");
      if (column.empty() || find_column(constraint.table, column) == nullptr ||
          !columns.insert(column).second) {
        throw Error{ErrorCode::database,
                    "PostgreSQL schema snapshot contains invalid constraint columns"};
      }
    }
    if ((constraint.kind == ConstraintKind::primary_key ||
         constraint.kind == ConstraintKind::unique ||
         constraint.kind == ConstraintKind::foreign_key) &&
        constraint.columns.empty()) {
      throw Error{ErrorCode::database, "PostgreSQL key constraint does not contain any columns"};
    }
    if (constraint.kind == ConstraintKind::not_null) {
      if (constraint.columns.size() != 1U ||
          !find_column(constraint.table, constraint.columns.front())->not_null) {
        throw Error{ErrorCode::database,
                    "PostgreSQL NOT NULL constraint is inconsistent with its column"};
      }
    }
    if (constraint.kind == ConstraintKind::primary_key &&
        std::ranges::any_of(constraint.columns, [&](const std::string& column) {
          return !find_column(constraint.table, column)->not_null;
        })) {
      throw Error{ErrorCode::database, "PostgreSQL primary-key columns must be marked NOT NULL"};
    }

    if (constraint.kind == ConstraintKind::foreign_key) {
      if (!constraint.referenced_table.has_value() ||
          !tables.contains(*constraint.referenced_table) ||
          constraint.referenced_columns.size() != constraint.columns.size()) {
        throw Error{ErrorCode::unsupported,
                    "PostgreSQL foreign key references an unmanaged or invalid table key"};
      }
      std::set<std::string, std::less<>> referenced_columns;
      for (const auto& column : constraint.referenced_columns) {
        require_no_nul(column, "PostgreSQL referenced constraint column");
        if (column.empty() || find_column(*constraint.referenced_table, column) == nullptr ||
            !referenced_columns.insert(column).second) {
          throw Error{ErrorCode::database,
                      "PostgreSQL schema snapshot contains invalid referenced columns"};
        }
      }
    } else if (constraint.referenced_table.has_value() || !constraint.referenced_columns.empty()) {
      throw Error{ErrorCode::database,
                  "non-foreign-key PostgreSQL constraint has referenced columns"};
    }
  }

  std::ranges::sort(snapshot.indexes, {}, &Index::name);
  std::set<QualifiedName> relation_names;
  for (const auto& table : snapshot.tables) {
    relation_names.insert(table.name);
  }
  for (const auto& index : snapshot.indexes) {
    require_no_nul(index.name.name, "PostgreSQL index name");
    require_no_nul(index.method, "PostgreSQL index method");
    if (!tables.contains(index.table) || index.name.schema != index.table.schema ||
        index.name.name.empty() || index.method.empty() || index.key_expressions.empty() ||
        !relation_names.insert(index.name).second || (index.nulls_not_distinct && !index.unique)) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains an invalid standalone index"};
    }
    for (const auto& expression : index.key_expressions) {
      require_no_nul(expression, "PostgreSQL index key expression");
      if (expression.empty()) {
        throw Error{ErrorCode::database, "PostgreSQL index key expression must not be empty"};
      }
    }
    std::set<std::string, std::less<>> included_columns;
    for (const auto& column : index.included_columns) {
      require_no_nul(column, "PostgreSQL included index column");
      if (column.empty() || find_column(index.table, column) == nullptr ||
          !included_columns.insert(column).second) {
        throw Error{ErrorCode::database,
                    "PostgreSQL schema snapshot contains invalid included index columns"};
      }
    }
    if (index.predicate.has_value()) {
      require_no_nul(*index.predicate, "PostgreSQL index predicate");
      if (index.predicate->empty()) {
        throw Error{ErrorCode::database, "PostgreSQL index predicate must not be empty"};
      }
    }
  }

  std::ranges::sort(snapshot.policies,
                    [](const RowSecurityPolicy& left, const RowSecurityPolicy& right) {
                      return std::tie(left.table, left.name) < std::tie(right.table, right.name);
                    });
  for (std::size_t index = 0U; index < snapshot.policies.size(); ++index) {
    auto& policy = snapshot.policies[index];
    if (index != 0U && policy.table == snapshot.policies[index - 1U].table &&
        policy.name == snapshot.policies[index - 1U].name) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains duplicate row-security policies"};
    }
    require_no_nul(policy.name, "PostgreSQL row-security policy name");
    if (!tables.contains(policy.table) || policy.name.empty() || policy.roles.empty()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL schema snapshot contains an invalid row-security policy"};
    }
    std::ranges::sort(policy.roles);
    if (std::ranges::adjacent_find(policy.roles) != policy.roles.end()) {
      throw Error{ErrorCode::database, "PostgreSQL row-security policy repeats a role"};
    }
    for (const auto& role : policy.roles) {
      require_no_nul(role.name, "PostgreSQL row-security policy role");
      if (role.public_role != role.name.empty()) {
        throw Error{ErrorCode::database, "PostgreSQL row-security policy has an invalid role"};
      }
      if (!role.public_role) {
        throw Error{ErrorCode::unsupported,
                    "PostgreSQL row-security policies may currently target only the PUBLIC role"};
      }
    }
    if (policy.using_expression.has_value()) {
      require_no_nul(*policy.using_expression, "PostgreSQL policy USING expression");
      if (policy.using_expression->empty()) {
        throw Error{ErrorCode::database, "PostgreSQL policy USING expression must not be empty"};
      }
    }
    if (policy.check_expression.has_value()) {
      require_no_nul(*policy.check_expression, "PostgreSQL policy WITH CHECK expression");
      if (policy.check_expression->empty()) {
        throw Error{ErrorCode::database,
                    "PostgreSQL policy WITH CHECK expression must not be empty"};
      }
    }
    if ((policy.command == PolicyCommand::insert && policy.using_expression.has_value()) ||
        ((policy.command == PolicyCommand::select ||
          policy.command == PolicyCommand::delete_rows) &&
         policy.check_expression.has_value())) {
      throw Error{ErrorCode::database, "PostgreSQL policy expressions do not match its command"};
    }
  }

  std::ranges::sort(snapshot.unsupported_objects);
  for (const auto& object : snapshot.unsupported_objects) {
    require_no_nul(object.kind, "PostgreSQL unsupported catalog kind");
    require_no_nul(object.identity, "PostgreSQL unsupported catalog identity");
    if (object.kind.empty() || object.identity.empty()) {
      throw Error{ErrorCode::database,
                  "PostgreSQL snapshot contains an invalid unsupported catalog object"};
    }
  }
  if (!snapshot.unsupported_objects.empty()) {
    const auto& object = snapshot.unsupported_objects.front();
    throw Error{ErrorCode::unsupported, "unsupported PostgreSQL " + object.kind + " " +
                                            object.identity + " exists in a managed schema"};
  }
  snapshot.semantic_hash = semantic_hash_normalized(snapshot);
  return snapshot;
}

std::string semantic_hash(const SchemaSnapshot& snapshot) {
  return normalize_snapshot(snapshot).semantic_hash;
}

} // namespace dbdiff::postgresql
