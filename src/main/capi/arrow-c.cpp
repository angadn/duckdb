#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"

using duckdb::ArrowConverter;
using duckdb::ArrowResultWrapper;
using duckdb::Connection;
using duckdb::DataChunk;
using duckdb::LogicalType;
using duckdb::MaterializedQueryResult;
using duckdb::PreparedStatementWrapper;
using duckdb::QueryResult;
using duckdb::QueryResultType;

duckdb_state duckdb_query_arrow(duckdb_connection connection, const char *query, duckdb_arrow *out_result) {
	Connection *conn = (Connection *)connection;
	auto wrapper = new ArrowResultWrapper();
	wrapper->result = conn->Query(query);
	*out_result = (duckdb_arrow)wrapper;
	return !wrapper->result->HasError() ? DuckDBSuccess : DuckDBError;
}

duckdb_state duckdb_query_arrow_schema(duckdb_arrow result, duckdb_arrow_schema *out_schema) {
	if (!out_schema) {
		return DuckDBSuccess;
	}
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	ArrowConverter::ToArrowSchema((ArrowSchema *)*out_schema, wrapper->result->types, wrapper->result->names,
	                              wrapper->timezone_config);
	return DuckDBSuccess;
}

duckdb_state duckdb_query_arrow_array(duckdb_arrow result, duckdb_arrow_array *out_array) {
	if (!out_array) {
		return DuckDBSuccess;
	}
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	auto success = wrapper->result->TryFetch(wrapper->current_chunk, wrapper->result->GetErrorObject());
	if (!success) { // LCOV_EXCL_START
		return DuckDBError;
	} // LCOV_EXCL_STOP
	if (!wrapper->current_chunk || wrapper->current_chunk->size() == 0) {
		return DuckDBSuccess;
	}
	ArrowConverter::ToArrowArray(*wrapper->current_chunk, reinterpret_cast<ArrowArray *>(*out_array));
	return DuckDBSuccess;
}

idx_t duckdb_arrow_row_count(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	if (wrapper->result->HasError()) {
		return 0;
	}
	return wrapper->result->RowCount();
}

idx_t duckdb_arrow_column_count(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	return wrapper->result->ColumnCount();
}

idx_t duckdb_arrow_rows_changed(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	if (wrapper->result->HasError()) {
		return 0;
	}
	idx_t rows_changed = 0;
	auto &collection = wrapper->result->Collection();
	idx_t row_count = collection.Count();
	if (row_count > 0 && wrapper->result->properties.return_type == duckdb::StatementReturnType::CHANGED_ROWS) {
		auto rows = collection.GetRows();
		D_ASSERT(row_count == 1);
		D_ASSERT(rows.size() == 1);
		rows_changed = rows[0].GetValue(0).GetValue<int64_t>();
	}
	return rows_changed;
}

const char *duckdb_query_arrow_error(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	return wrapper->result->GetError().c_str();
}

void duckdb_destroy_arrow(duckdb_arrow *result) {
	if (*result) {
		auto wrapper = reinterpret_cast<ArrowResultWrapper *>(*result);
		delete wrapper;
		*result = nullptr;
	}
}

duckdb_state duckdb_execute_prepared_arrow(duckdb_prepared_statement prepared_statement, duckdb_arrow *out_result) {
	auto wrapper = reinterpret_cast<PreparedStatementWrapper *>(prepared_statement);
	if (!wrapper || !wrapper->statement || wrapper->statement->HasError() || !out_result) {
		return DuckDBError;
	}
	auto arrow_wrapper = new ArrowResultWrapper();
	if (wrapper->statement->context->config.set_variables.find("TimeZone") ==
	    wrapper->statement->context->config.set_variables.end()) {
		arrow_wrapper->timezone_config = "UTC";
	} else {
		arrow_wrapper->timezone_config =
		    wrapper->statement->context->config.set_variables["TimeZone"].GetValue<std::string>();
	}

	auto result = wrapper->statement->Execute(wrapper->values, false);
	D_ASSERT(result->type == QueryResultType::MATERIALIZED_RESULT);
	arrow_wrapper->result = duckdb::unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(result));
	*out_result = reinterpret_cast<duckdb_arrow>(arrow_wrapper);
	return !arrow_wrapper->result->HasError() ? DuckDBSuccess : DuckDBError;
}

namespace arrow_array_stream_wrapper {
namespace {
struct PrivateData {
	ArrowSchema *schema;
	ArrowArray *array;
	bool done = false;
};

void EmptySchemaRelease(ArrowSchema *schema) {
}

void GetSchemaVoid(uintptr_t stream_factory_ptr, duckdb::ArrowSchemaWrapper &schema) {
	auto private_data = (PrivateData *)((ArrowArrayStream *)stream_factory_ptr)->private_data;
	schema.arrow_schema = *private_data->schema;

	// Need to nullify the root schema's release function here, because streams don't allow us to set the release
	// function. For the schema's children, we nullify the release functions in `duckdb_arrow_scan`, so we don't need to
	// handle them again here.
	schema.arrow_schema.release = EmptySchemaRelease;
}

int GetSchema(struct ArrowArrayStream *stream, struct ArrowSchema *out) {
	auto private_data = static_cast<arrow_array_stream_wrapper::PrivateData *>((stream->private_data));
	*out = *private_data->schema;
	return 0;
}

int GetNext(struct ArrowArrayStream *stream, struct ArrowArray *out) {
	auto private_data = static_cast<arrow_array_stream_wrapper::PrivateData *>((stream->private_data));
	*out = *private_data->array;
	if (private_data->done) {
		out->release(out);
	}

	private_data->done = true;
	return 0;
}

const char *GetLastError(struct ArrowArrayStream *stream) {
	return nullptr;
}

void Release(struct ArrowArrayStream *) {
}

duckdb_state Ingest(duckdb_connection connection, const char *table_name, struct ArrowArrayStream *input) {
	try {
		auto cconn = (duckdb::Connection *)connection;
		cconn
		    ->TableFunction("arrow_scan", {duckdb::Value::POINTER((uintptr_t)input),
		                                   duckdb::Value::POINTER((uintptr_t)input->get_next),
		                                   duckdb::Value::POINTER((uintptr_t)GetSchemaVoid)})
		    ->CreateView(table_name, true, true);
	} catch (const std::exception &exc) {
		// TODO: We should probably log this, but where?
		return DuckDBError;
	}

	return DuckDBSuccess;
}
} // namespace
} // namespace arrow_array_stream_wrapper

duckdb_state duckdb_arrow_scan(duckdb_connection connection, const char *table_name, duckdb_arrow_stream arrow) {
	auto stream = reinterpret_cast<ArrowArrayStream *>(arrow->__arrwstr);

	// Backup release functions - we nullify children schema release functions because we don't want to release on
	// behalf of the caller, downstream in our code. Note that Arrow releases target immediate children, but aren't
	// recursive. So we only back up immediate children here and restore their functions.
	ArrowSchema schema;
	stream->get_schema(stream, &schema);

	typedef void (*release_fn_t)(ArrowSchema *);
	std::vector<release_fn_t> release_fns(schema.n_children);
	for (int64_t i = 0; i < schema.n_children; i++) {
		auto child = schema.children[i];
		release_fns[i] = child->release;
		child->release = arrow_array_stream_wrapper::EmptySchemaRelease;
	}

	auto ret = arrow_array_stream_wrapper::Ingest(connection, table_name, stream);

	// Restore release functions.
	for (int64_t i = 0; i < schema.n_children; i++) {
		schema.children[i]->release = release_fns[i];
	}

	return ret;
}

duckdb_state duckdb_arrow_array_scan(duckdb_connection connection, const char *table_name,
                                     duckdb_arrow_schema arrow_schema, duckdb_arrow_array arrow_array) {
	arrow_array_stream_wrapper::PrivateData private_data;
	private_data.schema = reinterpret_cast<ArrowSchema *>(arrow_schema);
	private_data.array = reinterpret_cast<ArrowArray *>(arrow_array);
	private_data.done = false;

	ArrowArrayStream stream;
	stream.get_schema = arrow_array_stream_wrapper::GetSchema;
	stream.get_next = arrow_array_stream_wrapper::GetNext;
	stream.get_last_error = arrow_array_stream_wrapper::GetLastError;
	stream.release = arrow_array_stream_wrapper::Release;
	stream.private_data = &private_data;

	_duckdb_arrow_stream arrow_stream;
	arrow_stream.__arrwstr = &stream;
	return duckdb_arrow_scan(connection, table_name, &arrow_stream);
}
