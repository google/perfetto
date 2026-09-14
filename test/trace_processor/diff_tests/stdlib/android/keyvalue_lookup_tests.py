from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, TextProto


class KeyValueLookup(TestSuite):

  def test_keyvalue_lookup_extract_quoted(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.keyvalue_lookup;
        SELECT _android_keyvalue_lookup_extract_key_value_arg(
          'key1="value 1" key2="value 2" key3="value3"',
          'key2'
        ) AS val;
        """,
        out=Csv("""
        "val"
        "value 2"
        """))

  def test_keyvalue_lookup_extract_unquoted(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.keyvalue_lookup;
        SELECT _android_keyvalue_lookup_extract_key_value_arg(
          'key1=value1 key2=value2 key3=value3',
          'key2'
        ) AS val1,
        _android_keyvalue_lookup_extract_key_value_arg(
          '{key3=value3}',
          'key3'
        ) AS val2;
        """,
        out=Csv("""
        "val1","val2"
        "value2","value3"
        """))

  def test_keyvalue_lookup_extract_missing_or_null(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.keyvalue_lookup;
        SELECT _android_keyvalue_lookup_extract_key_value_arg(
          'key1=value1 key2=value2',
          'key3'
        ) AS val1,
        _android_keyvalue_lookup_extract_key_value_arg(
          'key1 key2',
          'key1'
        ) AS val2;
        """,
        out=Csv("""
        "val1","val2"
        "[NULL]","[NULL]"
        """))
