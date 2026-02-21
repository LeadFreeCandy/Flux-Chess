
import customtkinter as ctk

class Square(ctk.CTkFrame):
    def __init__(self, master, row, col, color):
        super().__init__(master, width=80, height=80, fg_color=color, corner_radius=0)
        self.grid_propagate(False)
        
        # Square
        self.row = row
        self.col = col
        self.default_color = color
        # self.callback = callback
        self.highlighted = False
        self.current_color = color

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
            self.current_color = self.default_color
        else:
            self.current_color = highlight_color
        self.configure(fg_color=self.current_color)
        self.highlighted = not self.highlighted
    
    def enable(self):
        # self.btn.state = "normal"
        self.configure(fg_color=self.current_color)
        # pws.set_opacity(self, value=1.0, color=self.cget("color"))
    
    def disable(self):
        # self.btn.state = "disabled"
        self.configure(fg_color="transparent")
        # pws.set_opacity(self, value=0.2, color=self.cget("color"))

class ChessBoard(ctk.CTkFrame):
    def __init__(self, master, rows=8, cols=8, boarders=True, diagonals=True):
        super().__init__(master)
        self.rows = rows
        self.cols = cols
        self.boarders = boarders
        self.diagonals = diagonals
        self.squares = {}
        self.square_size = 80
        
        self.nodes = []
        self.h_border_nodes = []
        self.v_border_nodes = []
        self.diagonal_nodes = []

        self._build_base()
        self._build_horizontal_borders()
        self._build_vertical_boarders()
        self._build_diagonals()
    
    # Normal Chess board
    def _build_base(self):
        for r in range(self.rows):
            for c in range(self.cols):
                color = "#ffb3b3" if (r + c) % 2 == 0 else "#b30000"
                square = Square(self, r, c, color)
                square.grid(row=r, column=c, sticky="nsew")
                
                self.nodes.append((square, r, c))
                self.squares[(r, c)] = square

    # Boarders
    def _build_horizontal_borders(self):
        if not self.boarders: return
        
        # Horizontal Borders
        for r in range(self.rows - 1):
            for c in range(self.cols):
                color = "#b3b3ff" if (r + c) % 2 == 0 else "#0000b3"

                square = Square(self, f"h_border_{r}", c, color)
                
                x_pos = (c * self.square_size) + (self.square_size / 2)
                y_pos = (r + 1) * self.square_size
                
                square.place(x=x_pos, y=y_pos, anchor="center")
                self.h_border_nodes.append((square, x_pos, y_pos))

    def _build_vertical_boarders(self):
        if not self.boarders: return

        # Vertical Borders
        for r in range(self.rows):
            for c in range(self.cols - 1):
                color = "#b3b3ff" if (r + c) % 2 == 0 else "#0000b3"
                square = Square(self, r, f"v_border_{c}", color)
                
                x_pos = (c + 1) * self.square_size
                y_pos = (r * self.square_size) + (self.square_size / 2)
                
                square.place(x=x_pos, y=y_pos, anchor="center")
                self.v_border_nodes.append((square, x_pos, y_pos))

    # Diagonals
    def _build_diagonals(self):
        if not self.diagonals: return
        
        for r in range(self.rows - 1):
            for c in range(self.cols - 1):
                color = "#b3ffb3" if (r + c) % 2 == 0 else "#008000"
                square = Square(self, f"diag_{r}", f"diag_{c}", color)
                
                x_pos = (c + 1) * self.square_size
                y_pos = (r + 1) * self.square_size
                
                square.place(x=x_pos, y=y_pos, anchor="center")
                self.diagonal_nodes.append((square, x_pos, y_pos))


    def set_layer_visibility(self, layer_level):
        """1 = Red Base, 2 = Blue Borders, 3 = Green Diagonals"""

        # Layer 1 (Blue Borders)
        if layer_level == 1:
            for square, x, y in self.nodes:
                # square.place(x=x, y=y, anchor="center")
                square.enable()
        else:
            for square, x, y in self.nodes:
                square.disable()

        # Layer 2 (Blue Borders)
        if layer_level == 2 and self.boarders:
            for square, x, y in self.h_border_nodes:
                # square.place(x=x, y=y, anchor="center")
                square.enable()
        else:
            for square, x, y in self.h_border_nodes:
                square.disable()

        # Layer 3 (Light Blue Diagonals)
        if layer_level == 3 and self.boarders:
            for square, x, y in self.v_border_nodes:
                # square.place(x=x, y=y, anchor="center")
                square.enable()
        else:
            for square, x, y in self.v_border_nodes:
                square.disable()
        
        # Layer 4 (Green Diagonals)
        if layer_level == 4 and self.diagonals:
            for square, x, y in self.diagonal_nodes:
                # square.place(x=x, y=y, anchor="center")
                square.enable()
        else:
            for square, x, y in self.diagonal_nodes:
                square.disable()


class DevPanel(ctk.CTkFrame):
    def __init__(self, master, controller):
        super().__init__(master, width=250)
        self.controller = controller
        
        self.title_label = ctk.CTkLabel(self, text="Developer Settings", font=("Arial", 18, "bold"))
        self.title_label.pack(pady=(20, 10), padx=20)

        self.layer_label = ctk.CTkLabel(self, text="Board Layer: 1", font=("Arial", 14))
        self.layer_label.pack(pady=(20, 0))

        self.slider = ctk.CTkSlider(
            self, from_=1, to=4, number_of_steps=3,
            command=self.on_slider_change
        )
        self.slider.set(1)
        self.slider.pack(pady=10, padx=20)

    def on_slider_change(self, value):
        layer_val = int(value)
        self.layer_label.configure(text=f"Board Layers: {layer_val}")
        self.controller.update_board_layers(layer_val)



class App(ctk.CTk):
    def __init__(self):
        super().__init__()
        self.title("Chessboard")
        self.geometry("1000x800")

        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(0, weight=1)

        self.board = ChessBoard(self, boarders=False, diagonals=True)
        self.board.grid(row=0, column=0, padx=40, pady=40, sticky="nsew")

        self.dev_panel = DevPanel(self, controller=self)
        self.dev_panel.grid(row=0, column=1, padx=(0, 40), pady=40, sticky="ns")

    # Connectivity between frames
    def update_board_layers(self, layer_level):
        self.board.set_layer_visibility(layer_level)

if __name__ == "__main__":
    app = App()
    app.mainloop()

