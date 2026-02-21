
import customtkinter as ctk

class Square(ctk.CTkFrame):
    def __init__(self, master, row, col, color, callback):
        super().__init__(master, width=80, height=80, fg_color=color, corner_radius=0)
        self.grid_propagate(False)
        
        # Square
        self.row = row
        self.col = col
        self.default_color = color
        self.callback = callback
        self.highlighted = False

        # self.piece_label = ctk.CTkLabel(
        #     self, 
        #     text="", 
        #     font=("Arial", 45),
        #     text_color="black"
        # )
        # self.piece_label.place(relx=0.5, rely=0.5, anchor="center")

        # Button
        self.btn = ctk.CTkButton(
            self,
            text="",
            fg_color="transparent",
            # hover_color="#f6f669",
            hover=False,
            width=80,
            height=80,
            corner_radius=0,
            border_width=0,
            command=self._on_button_press
        ) 
        self.btn.place(relx=0.5, rely=0.5, anchor="center")

    def _on_button_press(self):
        """Passes this specific square object back to the controller"""
        # self.callback(self)
        self.highlight()

    def highlight(self, highlight_color="#f6f669"):
        if self.highlighted:
            self.configure(fg_color=self.default_color)
        else:
            self.configure(fg_color=highlight_color)
        self.highlighted = not self.highlighted

class ChessBoard(ctk.CTkFrame):
    def __init__(self, master, rows=8, cols=8, boarders=False, diagonals=False):
        super().__init__(master)
        self.rows = rows
        self.cols = cols
        self.boarders = boarders
        self.diagonals = diagonals
        self.squares = {} # {(r, c): square}
    
        # Create the grid
        for r in range(self.rows):
            for c in range(self.cols):
                color = "#ebecd0" if (r + c) % 2 == 0 else "#36386D"

                square = Square(self, r, c, color, self.handle_square_click)

                square.grid(row=r, column=c, sticky="nsew")

                self.squares[(r, c)] = square
    
    def handle_square_click(self):
        pass



class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Chessboard GUI")
        self.board = ChessBoard(self)
        self.board.pack(padx=40, pady=40)


if __name__ == "__main__":
    app = App()
    app.mainloop()

