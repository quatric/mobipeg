import os


def pytest_addoption(parser):
    parser.addoption(
        "--update-refs",
        action="store_true",
        help="rewrite tests/mobiclip/ref/ from the current build",
    )


def pytest_configure(config):
    if config.getoption("--update-refs"):
        os.environ["MOBIPEG_UPDATE_REFS"] = "1"
