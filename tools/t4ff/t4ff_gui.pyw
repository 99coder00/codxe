"""Double click to open the t4ff converter window (Windows runs .pyw files without a console)."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from t4ff.gui import main  # noqa: E402

main()
