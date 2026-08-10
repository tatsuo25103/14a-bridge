"""Application entry point."""

import tkinter as tk

from gui import InfiniSolarGUI


def main() -> None:
    root = tk.Tk()
    InfiniSolarGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
