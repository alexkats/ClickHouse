import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

node1 = cluster.add_instance(
    "node1",
    stay_alive=True,
    main_configs=["configs/store_cleanup.xml"],
    with_remote_database_disk=False,  # The test checks data on the local disk
)

path_to_data = "/var/lib/clickhouse/"


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        yield cluster

    finally:
        cluster.shutdown()


def test_store_cleanup(started_cluster):
    sync_drop = {"database_atomic_wait_for_drop_and_detach_synchronously": True}

    node1.query("DROP DATABASE IF EXISTS db", settings=sync_drop)
    node1.query("DROP DATABASE IF EXISTS db2", settings=sync_drop)
    node1.query("DROP DATABASE IF EXISTS db3", settings=sync_drop)

    node1.query("CREATE DATABASE db UUID '10000000-1000-4000-8000-000000000001'")
    node1.query(
        "CREATE TABLE db.log UUID '10000000-1000-4000-8000-000000000002' ENGINE=Log AS SELECT 1"
    )
    node1.query(
        "CREATE TABLE db.mt UUID '10000000-1000-4000-8000-000000000003' ENGINE=MergeTree ORDER BY tuple() AS SELECT 1"
    )
    node1.query(
        "CREATE TABLE db.mem UUID '10000000-1000-4000-8000-000000000004' ENGINE=Memory AS SELECT 1"
    )

    node1.query("CREATE DATABASE db2 UUID '20000000-1000-4000-8000-000000000001'")
    node1.query(
        "CREATE TABLE db2.log UUID '20000000-1000-4000-8000-000000000002' ENGINE=Log AS SELECT 1"
    )
    node1.query("DETACH DATABASE db2", settings=sync_drop)

    node1.query("CREATE DATABASE db3 UUID '30000000-1000-4000-8000-000000000001'")
    node1.query(
        "CREATE TABLE db3.log UUID '30000000-1000-4000-8000-000000000002' ENGINE=Log AS SELECT 1"
    )
    node1.query(
        "CREATE TABLE db3.log2 UUID '30000000-1000-4000-8000-000000000003' ENGINE=Log AS SELECT 1"
    )
    node1.query("DETACH TABLE db3.log")
    node1.query("DETACH TABLE db3.log2 PERMANENTLY")

    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store"]
    )
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/100"]
    )
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/200"]
    )
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/300"]
    )

    node1.stop_clickhouse(kill=True)
    # All dirs related to `db` will be removed
    node1.exec_in_container(["rm", f"{path_to_data}/metadata/db.sql"])

    node1.exec_in_container(["mkdir", f"{path_to_data}/store/kek"])
    node1.exec_in_container(["touch", f"{path_to_data}/store/12"])
    try:
        node1.exec_in_container(["mkdir", f"{path_to_data}/store/456"])
    except Exception as e:
        print("Failed to create 456/:", str(e))
    node1.exec_in_container(["mkdir", f"{path_to_data}/store/456/testgarbage"])
    node1.exec_in_container(
        ["mkdir", f"{path_to_data}/store/456/30000000-1000-4000-8000-000000000003"]
    )
    node1.exec_in_container(
        ["touch", f"{path_to_data}/store/456/45600000-1000-4000-8000-000000000003"]
    )
    node1.exec_in_container(
        ["mkdir", f"{path_to_data}/store/456/45600000-1000-4000-8000-000000000004"]
    )

    node1.start_clickhouse()
    node1.query("DETACH DATABASE db2", settings=sync_drop)
    node1.query("DETACH TABLE db3.log")

    node1.wait_for_log_line(
        "Removing access rights for unused directory",
        timeout=60,
        look_behind_lines=1000000,
    )
    node1.wait_for_log_line(
        "directories from store", timeout=60, look_behind_lines=1000000
    )

    store = node1.exec_in_container(["ls", f"{path_to_data}/store"])
    assert "100" in store
    assert "200" in store
    assert "300" in store
    assert "456" in store
    assert "kek" in store
    assert "12" in store
    assert "d---------" in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store"]
    )
    assert "d---------" in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/456"]
    )

    # Metadata is removed, so store/100 contains garbage
    store100 = node1.exec_in_container(["ls", f"{path_to_data}/store/100"])
    assert "10000000-1000-4000-8000-000000000001" in store100
    assert "10000000-1000-4000-8000-000000000002" in store100
    assert "10000000-1000-4000-8000-000000000003" in store100
    assert "d---------" in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/100"]
    )

    # Database is detached, nothing to clean up
    store200 = node1.exec_in_container(["ls", f"{path_to_data}/store/200"])
    assert "20000000-1000-4000-8000-000000000001" in store200
    assert "20000000-1000-4000-8000-000000000002" in store200
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/200"]
    )

    # Tables are detached, nothing to clean up
    store300 = node1.exec_in_container(["ls", f"{path_to_data}/store/300"])
    assert "30000000-1000-4000-8000-000000000001" in store300
    assert "30000000-1000-4000-8000-000000000002" in store300
    assert "30000000-1000-4000-8000-000000000003" in store300
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/300"]
    )

    # Manually created garbage
    store456 = node1.exec_in_container(["ls", f"{path_to_data}/store/456"])
    assert "30000000-1000-4000-8000-000000000003" in store456
    assert "45600000-1000-4000-8000-000000000003" in store456
    assert "45600000-1000-4000-8000-000000000004" in store456
    assert "testgarbage" in store456
    assert "----------" in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/456"]
    )

    node1.wait_for_log_line(
        "Removing unused directory", timeout=90, look_behind_lines=1000000
    )
    node1.wait_for_log_line(
        "directories from store", timeout=90, look_behind_lines=1000000
    )
    node1.wait_for_log_line(
        "Nothing to clean up from store/", timeout=90, look_behind_lines=1000000
    )

    store = node1.exec_in_container(["ls", f"{path_to_data}/store"])
    assert "100" in store
    assert "200" in store
    assert "300" in store
    assert "456" in store
    assert "kek" not in store  # changed
    assert "\n12\n" not in store  # changed
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store"]
    )  # changed

    # Metadata is removed, so store/100 contains garbage
    store100 = node1.exec_in_container(["ls", f"{path_to_data}/store/100"])  # changed
    assert "10000000-1000-4000-8000-000000000001" not in store100  # changed
    assert "10000000-1000-4000-8000-000000000002" not in store100  # changed
    assert "10000000-1000-4000-8000-000000000003" not in store100  # changed
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/100"]
    )  # changed

    # Database is detached, nothing to clean up
    store200 = node1.exec_in_container(["ls", f"{path_to_data}/store/200"])
    assert "20000000-1000-4000-8000-000000000001" in store200
    assert "20000000-1000-4000-8000-000000000002" in store200
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/200"]
    )

    # Tables are detached, nothing to clean up
    store300 = node1.exec_in_container(["ls", f"{path_to_data}/store/300"])
    assert "30000000-1000-4000-8000-000000000001" in store300
    assert "30000000-1000-4000-8000-000000000002" in store300
    assert "30000000-1000-4000-8000-000000000003" in store300
    assert "d---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/300"]
    )

    # Manually created garbage
    store456 = node1.exec_in_container(["ls", f"{path_to_data}/store/456"])
    assert "30000000-1000-4000-8000-000000000003" not in store456  # changed
    assert "45600000-1000-4000-8000-000000000003" not in store456  # changed
    assert "45600000-1000-4000-8000-000000000004" not in store456  # changed
    assert "testgarbage" not in store456  # changed
    assert "---------" not in node1.exec_in_container(
        ["ls", "-l", f"{path_to_data}/store/456"]
    )  # changed

    node1.query("ATTACH TABLE db3.log2")
    node1.query("ATTACH DATABASE db2")
    node1.query("ATTACH TABLE db3.log")

    assert "1\n" == node1.query("SELECT * FROM db3.log")
    assert "1\n" == node1.query("SELECT * FROM db3.log2")
    assert "1\n" == node1.query("SELECT * FROM db2.log")
