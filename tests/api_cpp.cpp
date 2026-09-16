#include <piggle/piggle.hpp>

#define CHECK_IMPORT(name) \
	static_assert(&piggle::name == &::name, #name " must import the C API")

CHECK_IMPORT(pg_archive_builder_create);
CHECK_IMPORT(pg_archive_builder_write_all);
CHECK_IMPORT(pg_archive_builder_import);
CHECK_IMPORT(pg_archive_builder_copy);
CHECK_IMPORT(pg_archive_builder_finish);
CHECK_IMPORT(pg_archive_builder_close);
CHECK_IMPORT(pg_tree_watch);
CHECK_IMPORT(pg_tree_unwatch);
CHECK_IMPORT(pg_tree_poll);
CHECK_IMPORT(pg_context_open);
CHECK_IMPORT(pg_context_close);
CHECK_IMPORT(pg_name_normalize);
CHECK_IMPORT(pg_buffer_free);
CHECK_IMPORT(pg_file_read_all);
CHECK_IMPORT(pg_file_read_all_alloc);
CHECK_IMPORT(pg_file_write_all);
CHECK_IMPORT(pg_file_export);
CHECK_IMPORT(pg_file_inspect);
CHECK_IMPORT(pg_cursor_next);
CHECK_IMPORT(pg_file_verify);
CHECK_IMPORT(pg_file_delete);
CHECK_IMPORT(pg_file_close);
CHECK_IMPORT(pg_cursor_close);
CHECK_IMPORT(pg_reader_open_source);
CHECK_IMPORT(pg_reader_open_tree);
CHECK_IMPORT(pg_reader_open);
CHECK_IMPORT(pg_reader_open_native);
CHECK_IMPORT(pg_reader_inspect);
CHECK_IMPORT(pg_reader_read);
CHECK_IMPORT(pg_reader_close);
CHECK_IMPORT(pg_writer_open_source);
CHECK_IMPORT(pg_writer_open_file);
CHECK_IMPORT(pg_writer_open_archive_builder);
CHECK_IMPORT(pg_writer_open_native);
CHECK_IMPORT(pg_unpack_target_open);
CHECK_IMPORT(pg_writer_open_unpack);
CHECK_IMPORT(pg_unpack_target_close);
CHECK_IMPORT(pg_writer_write);
CHECK_IMPORT(pg_writer_finish);
CHECK_IMPORT(pg_writer_close);
CHECK_IMPORT(pg_source_open);
CHECK_IMPORT(pg_source_inspect);
CHECK_IMPORT(pg_source_read_all);
CHECK_IMPORT(pg_source_read_all_alloc);
CHECK_IMPORT(pg_source_write_all);
CHECK_IMPORT(pg_source_import);
CHECK_IMPORT(pg_source_copy);
CHECK_IMPORT(pg_source_export);
CHECK_IMPORT(pg_source_pack);
CHECK_IMPORT(pg_source_unpack);
CHECK_IMPORT(pg_source_find);
CHECK_IMPORT(pg_source_files);
CHECK_IMPORT(pg_source_request_subtree);
CHECK_IMPORT(pg_source_rescan);
CHECK_IMPORT(pg_source_validate);
CHECK_IMPORT(pg_source_recover);
CHECK_IMPORT(pg_source_delete);
CHECK_IMPORT(pg_source_close);
CHECK_IMPORT(pg_tree_create);
CHECK_IMPORT(pg_tree_inspect);
CHECK_IMPORT(pg_tree_open);
CHECK_IMPORT(pg_tree_attach);
CHECK_IMPORT(pg_tree_detach);
CHECK_IMPORT(pg_tree_source);
CHECK_IMPORT(pg_tree_read_all);
CHECK_IMPORT(pg_tree_read_all_alloc);
CHECK_IMPORT(pg_tree_export);
CHECK_IMPORT(pg_tree_pack);
CHECK_IMPORT(pg_tree_unpack);
CHECK_IMPORT(pg_tree_find);
CHECK_IMPORT(pg_tree_files);
CHECK_IMPORT(pg_tree_request_subtree);
CHECK_IMPORT(pg_tree_rescan);
CHECK_IMPORT(pg_tree_close);
CHECK_IMPORT(pg_write_options_init);

#undef CHECK_IMPORT

int main()
{
	piggle::context *context = 0;
	piggle::error error;
	if (piggle::pg_context_open(&context, &error) != piggle::PG_OK)
		return 1;
	if (piggle::pg_context_close(&context, &error) != piggle::PG_OK)
		return 1;
	return 0;
}
