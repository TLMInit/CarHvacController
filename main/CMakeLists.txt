# Every .c in main/ is part of the build: work packages add and remove files
# (including the stub_wpX.c placeholders) without touching this list.
idf_component_register(SRC_DIRS "."
                    INCLUDE_DIRS "."
                    EMBED_FILES "web/index.html")
